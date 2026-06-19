use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::engine::{BuildError, Task, TaskInput};

use super::{ExternalCompileConfig, ExternalOutputType, err};

pub(super) struct CompileExternalTask {
    pub key_str: String,
    pub config: ExternalCompileConfig,
    pub project_dir: PathBuf,
    pub build_dir: PathBuf,
    pub sdk_include: PathBuf,
    pub sdk_lib: PathBuf,
    pub sdk_bin: PathBuf,
    pub cart_state_include: PathBuf,
    pub objcopy: String,
    pub debug: bool,
    pub src_files: Vec<PathBuf>,
    pub output: PathBuf,
}

impl Task for CompileExternalTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        format!("external compile_command ({:?})", self.config.language)
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v: Vec<TaskInput> = self
            .src_files
            .iter()
            .map(|f| TaskInput::File(f.clone()))
            .collect();
        v.push(TaskInput::Value(self.config.command_template.clone()));
        v.push(TaskInput::Value(format!("debug={}", self.debug)));
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        compile_external(
            &self.config,
            &self.project_dir,
            &self.build_dir,
            &self.sdk_include,
            &self.sdk_lib,
            &self.sdk_bin,
            &self.cart_state_include,
            &self.objcopy,
            self.debug,
        )?;
        Ok(())
    }
}

pub(super) fn collect_external_source_files(
    project_dir: &Path,
    config: &ExternalCompileConfig,
) -> Result<Vec<PathBuf>, BuildError> {
    let ext = config.extension.as_str();
    let mut files = Vec::new();
    collect_files_by_extension(
        &project_dir.join("src/game").join(&config.language),
        ext,
        &mut files,
    )?;
    let lib_root = project_dir.join("src/lib");
    if lib_root.exists() {
        for entry in fs::read_dir(&lib_root)? {
            let entry = entry?;
            let path = entry.path();
            if path.is_dir() {
                collect_files_by_extension(&path, ext, &mut files)?;
            }
        }
    }
    files.sort();
    Ok(files)
}

pub(super) fn validate_compile_command_template(cmd: &str) -> Result<(), BuildError> {
    let has_obj = cmd.contains("@OBJFILE@");
    let has_lib = cmd.contains("@LIBFILE@");
    match (has_obj, has_lib) {
        (true, true) => {
            return Err(err(
                "blyt.build.yaml: compile_command cannot contain both @OBJFILE@ and @LIBFILE@",
            ));
        }
        (false, false) => {
            return Err(err(
                "blyt.build.yaml: compile_command must contain either @OBJFILE@ or @LIBFILE@",
            ));
        }
        _ => {}
    }
    if !cmd.contains("@SRCFILES@") {
        return Err(err(
            "blyt.build.yaml: compile_command must contain @SRCFILES@",
        ));
    }
    let mut rest = cmd;
    while let Some(at) = rest.find('@') {
        rest = &rest[at + 1..];
        if let Some(end) = rest.find('@') {
            let candidate = format!("@{}@", &rest[..end]);
            if !KNOWN_PLACEHOLDERS.contains(&candidate.as_str()) {
                return Err(err(format!(
                    "blyt.build.yaml: compile_command contains unknown placeholder {candidate:?}"
                )));
            }
            rest = &rest[end + 1..];
        }
    }
    Ok(())
}

const KNOWN_PLACEHOLDERS: &[&str] = &[
    "@SRCFILES@",
    "@OBJFILE@",
    "@LIBFILE@",
    "@SDK_INCLUDE@",
    "@SDK_LIB@",
    "@SDK_BIN@",
    "@DEBUG@",
    "@CART_GENERATED_C@",
];

fn collect_files_by_extension(
    dir: &Path,
    ext: &str,
    out: &mut Vec<PathBuf>,
) -> Result<(), BuildError> {
    if !dir.exists() {
        return Ok(());
    }
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_files_by_extension(&path, ext, out)?;
        } else if path.extension().and_then(OsStr::to_str) == Some(ext) {
            out.push(path);
        }
    }
    Ok(())
}

pub(super) fn tokenize_command(cmd: &str) -> Vec<String> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    let mut chars = cmd.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            ' ' | '\t' | '\n' | '\r' => {
                if !current.is_empty() {
                    tokens.push(std::mem::take(&mut current));
                }
            }
            '\'' => {
                for c in chars.by_ref() {
                    if c == '\'' {
                        break;
                    }
                    current.push(c);
                }
            }
            '"' => {
                for c in chars.by_ref() {
                    if c == '"' {
                        break;
                    }
                    current.push(c);
                }
            }
            _ => current.push(c),
        }
    }
    if !current.is_empty() {
        tokens.push(current);
    }
    tokens
}

fn compile_external(
    config: &ExternalCompileConfig,
    project_dir: &Path,
    build_dir: &Path,
    sdk_include: &Path,
    sdk_lib: &Path,
    sdk_bin: &Path,
    cart_state_include: &Path,
    objcopy: &str,
    debug: bool,
) -> Result<PathBuf, BuildError> {
    let src_files = collect_external_source_files(project_dir, config)?;

    let safe_name = config.language.replace(['/', '\\', ' '], "_");
    let (out_path, out_placeholder) = match config.output_type {
        ExternalOutputType::Object => (build_dir.join(format!("{safe_name}.o")), "@OBJFILE@"),
        ExternalOutputType::Archive => (build_dir.join(format!("{safe_name}.a")), "@LIBFILE@"),
    };

    let tokens = tokenize_command(&config.command_template);

    let out_str = out_path.to_string_lossy();
    let inc_str = sdk_include.to_string_lossy();
    let lib_str = sdk_lib.to_string_lossy();
    let bin_str = sdk_bin.to_string_lossy();
    let csi_str = cart_state_include.to_string_lossy();
    let debug_str = if debug { "1" } else { "0" };

    let mut argv: Vec<std::ffi::OsString> = Vec::new();
    for token in &tokens {
        if token == "@SRCFILES@" {
            for f in &src_files {
                argv.push(f.as_os_str().to_os_string());
            }
        } else {
            let expanded = token
                .replace(out_placeholder, &out_str)
                .replace("@SDK_INCLUDE@", &inc_str)
                .replace("@SDK_LIB@", &lib_str)
                .replace("@SDK_BIN@", &bin_str)
                .replace("@CART_GENERATED_C@", &csi_str)
                .replace("@DEBUG@", debug_str);
            argv.push(std::ffi::OsString::from(expanded));
        }
    }

    let status = Command::new(&argv[0])
        .args(&argv[1..])
        .current_dir(project_dir)
        .status()
        .map_err(|e| err(format!("failed to run compile_command {:?}: {e}", argv[0])))?;

    if !status.success() {
        return Err(err(format!(
            "compile_command failed for language {:?}",
            config.language
        )));
    }

    if !out_path.exists() {
        return Err(err(format!(
            "compile_command exited 0 but {out_placeholder} was not created: {}",
            out_path.display()
        )));
    }

    for section in &config.strip_sections {
        let status = Command::new(objcopy)
            .arg("--remove-section")
            .arg(section)
            .arg(&out_path)
            .status()
            .map_err(|e| err(format!("failed to run objcopy for strip_sections: {e}")))?;
        if !status.success() {
            return Err(err(format!("objcopy --remove-section {section:?} failed")));
        }
    }

    Ok(out_path)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::TaskInput;
    use tempfile::tempdir;

    fn make_task(
        src_files: Vec<PathBuf>,
        command_template: &str,
        debug: bool,
        output_type: ExternalOutputType,
        output: PathBuf,
    ) -> CompileExternalTask {
        CompileExternalTask {
            key_str: "ext/swift".to_string(),
            config: ExternalCompileConfig {
                language: "swift".to_string(),
                extension: "swift".to_string(),
                command_template: command_template.to_string(),
                strip_sections: vec![],
                output_type,
            },
            project_dir: PathBuf::from("/project"),
            build_dir: PathBuf::from("/build"),
            sdk_include: PathBuf::from("/sdk/include"),
            sdk_lib: PathBuf::from("/sdk/lib"),
            sdk_bin: PathBuf::from("/sdk/bin"),
            cart_state_include: PathBuf::from("/build/blyt/c"),
            objcopy: "llvm-objcopy".to_string(),
            debug,
            src_files,
            output,
        }
    }

    #[test]
    fn inputs_contains_src_files_and_template() {
        let d = tempdir().unwrap();
        let f1 = d.path().join("main.swift");
        let out = d.path().join("swift.o");
        let task = make_task(
            vec![f1.clone()],
            "swiftc -o @OBJFILE@ @SRCFILES@",
            false,
            ExternalOutputType::Object,
            out,
        );
        let inputs = task.inputs();
        assert!(inputs.contains(&TaskInput::File(f1)));
        assert!(inputs.contains(&TaskInput::Value(
            "swiftc -o @OBJFILE@ @SRCFILES@".to_string()
        )));
    }

    #[test]
    fn inputs_debug_flag_distinguishes_debug_from_release() {
        let d = tempdir().unwrap();
        let out = d.path().join("swift.o");
        let debug_task = make_task(
            vec![],
            "swiftc -o @OBJFILE@ @SRCFILES@",
            true,
            ExternalOutputType::Object,
            out.clone(),
        );
        let release_task = make_task(
            vec![],
            "swiftc -o @OBJFILE@ @SRCFILES@",
            false,
            ExternalOutputType::Object,
            out,
        );
        assert!(
            debug_task
                .inputs()
                .contains(&TaskInput::Value("debug=true".to_string()))
        );
        assert!(
            release_task
                .inputs()
                .contains(&TaskInput::Value("debug=false".to_string()))
        );
        assert_ne!(debug_task.inputs(), release_task.inputs());
    }

    #[test]
    fn outputs_is_single_path() {
        let d = tempdir().unwrap();
        let out = d.path().join("swift.o");
        let task = make_task(
            vec![],
            "swiftc -o @OBJFILE@ @SRCFILES@",
            false,
            ExternalOutputType::Object,
            out.clone(),
        );
        assert_eq!(task.outputs(), vec![out]);
    }
}
