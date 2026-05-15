use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

/* -------------------------------------------------------------------------
 * .cart.info section data
 *
 * 8-byte preamble (ADR-0073): "CINF" + format_major(u16le=0) + format_minor(u16le=0)
 * followed by the FlatBuffers CartInfo table with api_version_major=0 and
 * api_version_minor=0 (both fields explicitly set; 12 bytes as produced by
 * flatcc for this schema).
 * ------------------------------------------------------------------------- */
const CART_INFO: &[u8] = &[
    // Preamble
    b'C', b'I', b'N', b'F', 0x00, 0x00, 0x00, 0x00,
    // FlatBuffers CartInfo(api_version_major=0, api_version_minor=0)
    0x04, 0x00, 0x00, 0x00, 0xfc, 0xff, 0xff, 0xff, 0x04, 0x00, 0x04, 0x00,
];

/* -------------------------------------------------------------------------
 * Linker script
 *
 * Produces a static RV32 ELF that passes the cart load-time security checks:
 *   - Entry point (blyt_main) in an executable PT_LOAD segment
 *   - No PT_INTERP (custom-loader path)
 *   - PT_GNU_RELRO present (covers .rodata)
 *   - No ecall/ebreak in executable segments — cart code calls blyt API via
 *     direct JAL to the linker-defined trampoline addresses
 *
 * Blyt API functions are defined at their runtime trampoline addresses.
 * The runtime injects these trampolines into emulated memory before
 * execution starts (see src/libblyt/ecall.h for the address layout).
 * ------------------------------------------------------------------------- */
const LINKER_SCRIPT: &str = "
ENTRY(blyt_main)

/* Runtime-injected trampolines — addresses written by the runtime into
 * emulated memory before blyt_main is called (ecall.h BLYT_TRAMPOLINE_*). */
blyt_console_debug = 0x300C;

PHDRS {
    text    PT_LOAD      FLAGS(5);  /* r-x: code */
    rodata  PT_LOAD      FLAGS(4);  /* r--: read-only data */
    relro   PT_GNU_RELRO FLAGS(4);  /* RELRO covers .rodata */
    data    PT_LOAD      FLAGS(6);  /* rw-: mutable data */
}

SECTIONS {
    . = 0x10000;

    .text : ALIGN(4) {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);
    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
    } :rodata :relro

    . = ALIGN(4096);
    .data : ALIGN(4) {
        *(.data .data.*)
    } :data
    .bss (NOLOAD) : ALIGN(4) {
        *(.bss .bss.*)
    } :data
}
";

/* -------------------------------------------------------------------------
 * Error type
 * ------------------------------------------------------------------------- */

#[derive(Debug)]
pub struct BuildError(String);

impl std::fmt::Display for BuildError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}", self.0)
    }
}

impl From<std::io::Error> for BuildError {
    fn from(e: std::io::Error) -> Self {
        BuildError(e.to_string())
    }
}

fn err(msg: impl Into<String>) -> BuildError {
    BuildError(msg.into())
}

/* -------------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------------- */

pub fn run(project_dir: &Path, output: Option<&Path>) -> Result<(), BuildError> {
    let clang = std::env::var("BLYT_CLANG").unwrap_or_else(|_| "clang".to_string());
    let objcopy = std::env::var("BLYT_OBJCOPY").unwrap_or_else(|_| "llvm-objcopy".to_string());

    let sdk_include = find_sdk_include()?;

    let c_src_dir = project_dir.join("src/game/c");
    let c_files = collect_c_files(&c_src_dir)?;
    if c_files.is_empty() {
        return Err(err(format!(
            "no .c files found under {}",
            c_src_dir.display()
        )));
    }

    let build_dir = project_dir.join("build/game/c");
    fs::create_dir_all(&build_dir)?;

    let ld_script = build_dir.join("blyt_cart.ld");
    fs::write(&ld_script, LINKER_SCRIPT)?;

    let cart_info_file = build_dir.join("cart.info.bin");
    fs::write(&cart_info_file, CART_INFO)?;

    let mut obj_files = Vec::new();
    for src in &c_files {
        let obj = compile_c(&clang, src, &build_dir, &sdk_include)?;
        obj_files.push(obj);
    }

    let raw_elf = build_dir.join("cart.elf");
    link_cart(&clang, &obj_files, &ld_script, &raw_elf)?;

    let output_path = output
        .map(PathBuf::from)
        .unwrap_or_else(|| default_output(project_dir));

    finalise_cart(&objcopy, &raw_elf, &cart_info_file, &output_path)?;

    println!("built: {}", output_path.display());
    Ok(())
}

fn default_output(project_dir: &Path) -> PathBuf {
    let name = project_dir
        .file_name()
        .and_then(OsStr::to_str)
        .unwrap_or("cart");
    project_dir.with_file_name(format!("{name}.blyt"))
}

/* -------------------------------------------------------------------------
 * SDK include directory
 *
 * Looks for the directory containing blyt.h in order:
 *   1. $BLYT_SDK_DIR/include  (or $BLYT_SDK_DIR if blyt.h is directly inside)
 *   2. Ancestors of the running binary (for dev: devtool/target/.../blyt
 *      → the repo root has include/blyt.h)
 * ------------------------------------------------------------------------- */

fn find_sdk_include() -> Result<PathBuf, BuildError> {
    if let Ok(sdk) = std::env::var("BLYT_SDK_DIR") {
        let sdk = PathBuf::from(sdk);
        let via_include = sdk.join("include");
        if via_include.join("blyt.h").exists() {
            return Ok(via_include);
        }
        if sdk.join("blyt.h").exists() {
            return Ok(sdk);
        }
        return Err(err(format!(
            "BLYT_SDK_DIR={} does not contain include/blyt.h",
            sdk.display()
        )));
    }

    if let Ok(exe) = std::env::current_exe() {
        for ancestor in exe.ancestors().skip(1) {
            let candidate = ancestor.join("include").join("blyt.h");
            if candidate.exists() {
                return Ok(ancestor.join("include"));
            }
        }
    }

    Err(err(
        "cannot find blyt.h — set BLYT_SDK_DIR to the blyt repository root",
    ))
}

/* -------------------------------------------------------------------------
 * Source file discovery — all .c files under dir, recursively
 * ------------------------------------------------------------------------- */

fn collect_c_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    collect_c_recursive(dir, &mut files)?;
    files.sort();
    Ok(files)
}

fn collect_c_recursive(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), BuildError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_c_recursive(&path, out)?;
        } else if path.extension().and_then(OsStr::to_str) == Some("c") {
            out.push(path);
        }
    }
    Ok(())
}

/* -------------------------------------------------------------------------
 * Compilation: one .c → one .o
 * ------------------------------------------------------------------------- */

fn compile_c(
    clang: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
) -> Result<PathBuf, BuildError> {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let obj = build_dir.join(format!("{stem}.o"));

    let status = Command::new(clang)
        .args([
            "--target=riscv32",
            "-march=rv32imafc",
            "-mabi=ilp32f",
            "-nostdlib",
            "-fno-exceptions",
            "-fno-plt",
            "-c",
        ])
        .arg("-I")
        .arg(sdk_include)
        .arg("-o")
        .arg(&obj)
        .arg(src)
        .status()
        .map_err(|e| err(format!("failed to run {clang}: {e}")))?;

    if !status.success() {
        return Err(err(format!("compilation failed: {}", src.display())));
    }
    Ok(obj)
}

/* -------------------------------------------------------------------------
 * Linking: .o files → raw ELF
 * ------------------------------------------------------------------------- */

fn link_cart(
    clang: &str,
    objs: &[PathBuf],
    ld_script: &Path,
    output: &Path,
) -> Result<(), BuildError> {
    let mut cmd = Command::new(clang);
    cmd.args([
        "--target=riscv32",
        "-march=rv32imafc",
        "-mabi=ilp32f",
        "-nostdlib",
        "-fuse-ld=lld",
        // Explicit PHDRS in the linker script provides GNU_RELRO; disable
        // lld's automatic -z,relro to avoid a duplicate PT_GNU_RELRO segment.
        "-Wl,-z,norelro",
        "-Wl,--build-id=none",
    ])
    .arg(format!("-T{}", ld_script.display()))
    .arg("-o")
    .arg(output);

    for obj in objs {
        cmd.arg(obj);
    }

    let status = cmd
        .status()
        .map_err(|e| err(format!("failed to run {clang}: {e}")))?;

    if !status.success() {
        return Err(err("link failed"));
    }
    Ok(())
}

/* -------------------------------------------------------------------------
 * Cart finalisation: inject .cart.info, strip toolchain metadata
 *
 * .riscv.attributes is emitted by clang/lld but not needed at runtime.
 * .comment contains toolchain version strings; no value in a cart binary.
 * Both are stripped to keep the cart clean and avoid triggering the loader's
 * unknown-section check (.riscv.attributes is also in the known-sections list
 * in cart_load.c, but removing it is still good hygiene).
 * ------------------------------------------------------------------------- */

fn finalise_cart(
    objcopy: &str,
    raw_elf: &Path,
    cart_info_file: &Path,
    output: &Path,
) -> Result<(), BuildError> {
    let status = Command::new(objcopy)
        .arg("--add-section")
        .arg(format!(".cart.info={}", cart_info_file.display()))
        .arg("--set-section-flags")
        .arg(".cart.info=alloc,readonly")
        .arg("--remove-section=.riscv.attributes")
        .arg("--remove-section=.comment")
        .arg(raw_elf)
        .arg(output)
        .status()
        .map_err(|e| err(format!("failed to run {objcopy}: {e}")))?;

    if !status.success() {
        return Err(err("objcopy (finalise) failed"));
    }
    Ok(())
}
