use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::engine::{BuildError, Task, TaskInput, parse_depfile};

use super::{CompileEntry, err, path_str};

pub(super) const CPP_TARGET_FLAGS: &[&str] = &[
    "--target=riscv32",
    "-march=rv32imafdc",
    "-mabi=ilp32d",
    "-nostdlib",
    "-fno-exceptions",
    "-fno-rtti",
    "-fpie",
    "-ffunction-sections",
    "-fdata-sections",
    "-ffp-contract=off",
    "-fno-fast-math",
    "-fwrapv",
    "-frounding-math",
    "-fsignaling-nans",
];

struct CompileCppTask {
    key_str: String,
    clangpp: String,
    src: PathBuf,
    build_dir: PathBuf,
    sdk_include: PathBuf,
    libcxx_include: PathBuf,
    extra_includes: Vec<PathBuf>,
    debug_flags: Vec<String>,
    project_dir: PathBuf,
}

impl Task for CompileCppTask {
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
        v.push(TaskInput::Value(self.clangpp.clone()));
        for f in &self.debug_flags {
            v.push(TaskInput::Value(f.clone()));
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
        compile_cpp(
            &self.clangpp,
            &self.src,
            &self.build_dir,
            &self.sdk_include,
            &self.libcxx_include,
            &inc_refs,
            &self.debug_flags,
        )?;
        Ok(())
    }
}

pub(super) fn make_cpp_task(
    project_dir: &Path,
    src: PathBuf,
    build_dir: PathBuf,
    clangpp: &str,
    sdk_include: &Path,
    libcxx_include: PathBuf,
    extra_includes: Vec<PathBuf>,
    debug_flags: Vec<String>,
    key_prefix: &str,
) -> Box<dyn Task> {
    let rel = src.strip_prefix(project_dir).unwrap_or(&src);
    let key_str = format!("{key_prefix}/{}", rel.display());
    Box::new(CompileCppTask {
        key_str,
        clangpp: clangpp.to_string(),
        src,
        build_dir,
        sdk_include: sdk_include.to_path_buf(),
        libcxx_include,
        extra_includes,
        debug_flags,
        project_dir: project_dir.to_path_buf(),
    })
}

pub(super) fn find_clangpp() -> String {
    if let Ok(c) = std::env::var("BLYT_CLANGPP") {
        return c;
    }
    for sdk in super::sdk_candidates() {
        let p = sdk.join("bin/blyt-clang++");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "clang++".to_string()
}

pub(super) fn collect_cpp_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    collect_cpp_recursive(dir, &mut files)?;
    files.sort();
    Ok(files)
}

fn collect_cpp_recursive(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), BuildError> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_cpp_recursive(&path, out)?;
        } else if matches!(
            path.extension().and_then(OsStr::to_str),
            Some("cpp" | "cxx" | "cc")
        ) {
            out.push(path);
        }
    }
    Ok(())
}

fn compile_cpp(
    clangpp: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    libcxx_include: &Path,
    extra_includes: &[&Path],
    debug_flags: &[String],
) -> Result<PathBuf, BuildError> {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let obj = build_dir.join(format!("{stem}.o"));

    let mut cmd = Command::new(clangpp);
    cmd.args(CPP_TARGET_FLAGS)
        .arg("-c")
        .arg("-MD")
        .arg("-MF")
        .arg(build_dir.join(format!("{stem}.d")))
        // Both paths must be -isystem so they are in the same search group; within
        // that group, command-line order applies.  Mixing -I (user) and -isystem
        // (system) puts all -I paths first regardless of position, so libcxx headers
        // would always lose to the musl headers if the musl path uses -I.
        .arg("-isystem")
        .arg(libcxx_include)
        .arg("-isystem")
        .arg(sdk_include);

    for inc in extra_includes {
        cmd.arg("-I").arg(inc);
    }
    for flag in debug_flags {
        cmd.arg(flag);
    }

    let status = cmd
        .arg("-o")
        .arg(&obj)
        .arg(src)
        .status()
        .map_err(|e| err(format!("failed to run {clangpp}: {e}")))?;

    if !status.success() {
        return Err(err(format!("compilation failed: {}", src.display())));
    }
    Ok(obj)
}

pub(super) fn cpp_compile_arguments(
    clangpp: &str,
    src: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    libcxx_include: &Path,
    extra_includes: &[&Path],
    debug_flags: &[String],
) -> CompileEntry {
    let stem = src.file_stem().and_then(OsStr::to_str).unwrap_or("unknown");
    let mut a: Vec<String> = vec![clangpp.to_string()];
    a.extend(CPP_TARGET_FLAGS.iter().map(|f| f.to_string()));
    a.push("-c".into());
    a.push("-isystem".into());
    a.push(path_str(libcxx_include));
    a.push("-isystem".into());
    a.push(path_str(sdk_include));
    for inc in extra_includes {
        a.push("-I".into());
        a.push(path_str(inc));
    }
    a.extend(debug_flags.iter().cloned());
    a.push("-o".into());
    a.push(path_str(&build_dir.join(format!("{stem}.o"))));
    a.push(path_str(src));
    CompileEntry {
        file: src.to_path_buf(),
        arguments: a,
    }
}
