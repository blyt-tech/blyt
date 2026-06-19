use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::engine::{BuildError, Task, TaskInput, parse_depfile};

use super::{CompileEntry, err, path_str};

/* -------------------------------------------------------------------------
 * Fixed codegen flags for cart C/C++ compilation (target, ABI, IEEE/determinism).
 * Shared by compile_c/compile_cpp and the compile_commands.json emitter so
 * clangd sees exactly the flags the cart is built with.  Excludes -c,
 * includes, defines, and per-variant flags, which callers append.
 * ------------------------------------------------------------------------- */

pub(super) const C_TARGET_FLAGS: &[&str] = &[
    "--target=riscv32",
    "-march=rv32imafdc",
    "-mabi=ilp32d",
    "-nostdlib",
    "-fno-exceptions",
    "-fpie",
    "-ffunction-sections",
    "-fdata-sections",
    "-ffp-contract=off",
    "-fno-fast-math",
    "-fwrapv",
    "-frounding-math",
    "-fsignaling-nans",
];

struct CompileCTask {
    key_str: String,
    clang: String,
    src: PathBuf,
    build_dir: PathBuf,
    sdk_include: PathBuf,
    extra_includes: Vec<PathBuf>,
    extra_defines: Vec<String>,
    debug_flags: Vec<String>,
    project_dir: PathBuf,
}

impl Task for CompileCTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        let rel = self
            .src
            .strip_prefix(&self.project_dir)
            .unwrap_or(&self.src);
        format!("compile  {}", rel.display())
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let stem = self
            .src
            .file_stem()
            .and_then(OsStr::to_str)
            .unwrap_or("unknown");
        let depfile = self.build_dir.join(format!("{stem}.d"));
        let mut v = vec![TaskInput::File(self.src.clone())];
        for dep in parse_depfile(&depfile) {
            v.push(TaskInput::File(dep));
        }
        v.push(TaskInput::Value(self.clang.clone()));
        for f in &self.debug_flags {
            v.push(TaskInput::Value(f.clone()));
        }
        for d in &self.extra_defines {
            v.push(TaskInput::Value(d.clone()));
        }
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        let stem = self
            .src
            .file_stem()
            .and_then(OsStr::to_str)
            .unwrap_or("unknown");
        vec![self.build_dir.join(format!("{stem}.o"))]
    }
    fn run(&self) -> Result<(), BuildError> {
        let inc_refs: Vec<&Path> = self.extra_includes.iter().map(PathBuf::as_path).collect();
        compile_c(
            &self.clang,
            &self.src,
            &self.build_dir,
            &self.sdk_include,
            &inc_refs,
            &self.extra_defines,
            &self.debug_flags,
        )?;
        Ok(())
    }
}

pub(super) fn make_c_task(
    project_dir: &Path,
    src: PathBuf,
    build_dir: PathBuf,
    clang: &str,
    sdk_include: &Path,
    extra_includes: Vec<PathBuf>,
    extra_defines: Vec<String>,
    debug_flags: Vec<String>,
    key_prefix: &str,
) -> Box<dyn Task> {
    let rel = src.strip_prefix(project_dir).unwrap_or(&src);
    let key_str = format!("{key_prefix}/{}", rel.display());
    Box::new(CompileCTask {
        key_str,
        clang: clang.to_string(),
        src,
        build_dir,
        sdk_include: sdk_include.to_path_buf(),
        extra_includes,
        extra_defines,
        debug_flags,
        project_dir: project_dir.to_path_buf(),
    })
}

pub(super) fn find_clang() -> String {
    if let Ok(c) = std::env::var("BLYT_CLANG") {
        return c;
    }
    for sdk in super::sdk_candidates() {
        let p = sdk.join("bin/blyt-clang");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "clang".to_string()
}

pub(super) fn collect_c_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
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

fn compile_c(
    clang: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    extra_includes: &[&Path],
    extra_defines: &[String],
    debug_flags: &[String],
) -> Result<PathBuf, BuildError> {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let obj = build_dir.join(format!("{stem}.o"));

    let mut cmd = Command::new(clang);
    cmd.args(C_TARGET_FLAGS)
        .arg("-c")
        .arg("-MD")
        .arg("-MF")
        .arg(build_dir.join(format!("{stem}.d")))
        .arg("-I")
        .arg(sdk_include);

    for inc in extra_includes {
        cmd.arg("-I").arg(inc);
    }
    for def in extra_defines {
        cmd.arg(def);
    }
    for flag in debug_flags {
        cmd.arg(flag);
    }

    let status = cmd
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

pub(super) fn c_compile_arguments(
    clang: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    extra_includes: &[&Path],
    extra_defines: &[String],
    debug_flags: &[String],
) -> CompileEntry {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let mut a: Vec<String> = vec![clang.to_string()];
    a.extend(C_TARGET_FLAGS.iter().map(|f| f.to_string()));
    a.push("-c".into());
    a.push("-I".into());
    a.push(path_str(sdk_include));
    for inc in extra_includes {
        a.push("-I".into());
        a.push(path_str(inc));
    }
    a.extend(extra_defines.iter().cloned());
    a.extend(debug_flags.iter().cloned());
    a.push("-o".into());
    a.push(path_str(&build_dir.join(format!("{stem}.o"))));
    a.push(path_str(src));
    CompileEntry {
        file: src.to_path_buf(),
        arguments: a,
    }
}
