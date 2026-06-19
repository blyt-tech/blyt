use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use crate::engine::{BuildError, Task, TaskInput};

use super::err;

pub(super) struct CompileLuaTask {
    pub luac: String,
    pub lua_files: Vec<PathBuf>,
    pub project_dir: PathBuf,
    pub output: PathBuf,
}

impl Task for CompileLuaTask {
    fn key(&self) -> &str {
        "lua_bytecode"
    }
    fn label(&self) -> String {
        "luac     src/game/lua/".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        let mut v: Vec<TaskInput> = self
            .lua_files
            .iter()
            .map(|f| TaskInput::File(f.clone()))
            .collect();
        v.push(TaskInput::Value(self.luac.clone()));
        v
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        let mut cmd = Command::new(&self.luac);
        cmd.arg("-o")
            .arg(&self.output)
            .arg("-P")
            .arg(&self.project_dir);
        for f in &self.lua_files {
            cmd.arg(f);
        }
        let status = cmd
            .status()
            .map_err(|e| err(format!("failed to run {}: {e}", self.luac)))?;
        if !status.success() {
            return Err(err("luac compilation failed"));
        }
        Ok(())
    }
}

pub(super) struct GenerateLuaDataTask {
    pub bytecode_path: PathBuf,
    pub output_c: PathBuf,
}

impl Task for GenerateLuaDataTask {
    fn key(&self) -> &str {
        "gen_lua_data"
    }
    fn label(&self) -> String {
        "gen      cart_lua_data.c".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        vec![TaskInput::File(self.bytecode_path.clone())]
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output_c.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        generate_lua_data_c(&self.bytecode_path, &self.output_c)
    }
}

pub(super) fn find_luac() -> String {
    if let Ok(c) = std::env::var("BLYT_LUAC") {
        return c;
    }
    for sdk in super::sdk_candidates() {
        let p = sdk.join("bin/blyt-luac");
        if p.exists() {
            return p.to_string_lossy().into_owned();
        }
    }
    "luac".to_string()
}

pub(super) fn collect_lua_files(dir: &Path) -> Result<Vec<PathBuf>, BuildError> {
    if !dir.exists() {
        return Ok(Vec::new());
    }
    let mut files = Vec::new();
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        if path.extension().map(|e| e == "lua").unwrap_or(false) {
            files.push(path);
        }
    }
    files.sort();
    Ok(files)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::TaskInput;
    use std::fs;
    use tempfile::tempdir;

    #[test]
    fn compile_lua_inputs_contains_files_and_compiler() {
        let d = tempdir().unwrap();
        let f1 = d.path().join("main.lua");
        let f2 = d.path().join("util.lua");
        fs::write(&f1, "").unwrap();
        fs::write(&f2, "").unwrap();
        let out = d.path().join("cart.luac");
        let task = CompileLuaTask {
            luac: "luac".to_string(),
            lua_files: vec![f1.clone(), f2.clone()],
            project_dir: d.path().to_path_buf(),
            output: out.clone(),
        };
        let inputs = task.inputs();
        assert!(inputs.contains(&TaskInput::File(f1)));
        assert!(inputs.contains(&TaskInput::File(f2)));
        assert!(inputs.contains(&TaskInput::Value("luac".to_string())));
    }

    #[test]
    fn compile_lua_outputs() {
        let d = tempdir().unwrap();
        let out = d.path().join("cart.luac");
        let task = CompileLuaTask {
            luac: "luac".to_string(),
            lua_files: vec![],
            project_dir: d.path().to_path_buf(),
            output: out.clone(),
        };
        assert_eq!(task.outputs(), vec![out]);
    }

    #[test]
    fn generate_lua_data_inputs_and_outputs() {
        let d = tempdir().unwrap();
        let bc = d.path().join("cart.luac");
        let out_c = d.path().join("cart_lua_data.c");
        let task = GenerateLuaDataTask {
            bytecode_path: bc.clone(),
            output_c: out_c.clone(),
        };
        assert_eq!(task.inputs(), vec![TaskInput::File(bc)]);
        assert_eq!(task.outputs(), vec![out_c]);
    }

    #[test]
    fn generate_lua_data_c_hex_format() {
        let d = tempdir().unwrap();
        let bc = d.path().join("cart.luac");
        fs::write(&bc, b"\x01\x02\x03").unwrap();
        let out_c = d.path().join("cart_lua_data.c");
        generate_lua_data_c(&bc, &out_c).unwrap();
        let src = fs::read_to_string(&out_c).unwrap();
        assert!(src.contains("0x01,"), "byte 1 hex");
        assert!(src.contains("0x02,"), "byte 2 hex");
        assert!(src.contains("0x03,"), "byte 3 hex");
        assert!(src.contains("cart_lua_bytecode_size = 3u"), "size symbol");
    }
}

fn generate_lua_data_c(bytecode_path: &Path, output_c: &Path) -> Result<(), BuildError> {
    let bytecode = fs::read(bytecode_path)?;
    let mut src = String::with_capacity(bytecode.len() * 5 + 128);
    src.push_str("/* Generated by blyt build — do not edit. */\n");
    src.push_str("const unsigned char cart_lua_bytecode[] = {\n");
    for (i, b) in bytecode.iter().enumerate() {
        if i % 16 == 0 {
            src.push_str("    ");
        }
        src.push_str(&format!("0x{b:02x},"));
        if i % 16 == 15 {
            src.push('\n');
        }
    }
    if !bytecode.is_empty() && bytecode.len() % 16 != 0 {
        src.push('\n');
    }
    src.push_str("};\n");
    src.push_str(&format!(
        "const unsigned int cart_lua_bytecode_size = {}u;\n",
        bytecode.len()
    ));
    fs::write(output_c, src)?;
    Ok(())
}
