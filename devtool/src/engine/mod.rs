mod fingerprint;
mod runner;

pub use fingerprint::parse_depfile;
pub use runner::run_tasks;

use std::path::PathBuf;

/// A single input to a build task.
pub enum TaskInput {
    /// A file whose content is tracked via size+mtime fast path and xxh3 slow path.
    File(PathBuf),
    /// A configuration value (flags, version strings, env vars) hashed directly.
    Value(String),
}

/// A named, independently-executable build step with declared inputs and outputs.
pub trait Task {
    /// Stable, path-like key used to locate the task's state file under
    /// `build/.blyt-tasks/<variant>/`. E.g. `"compile_c/src/game/c/player.c"`.
    fn key(&self) -> &str;

    /// Short label printed when the task runs. E.g. `"compile  src/game/c/player.c"`.
    fn label(&self) -> String;

    /// All inputs this task depends on. Headers from depfiles should be included here.
    fn inputs(&self) -> Vec<TaskInput>;

    /// All output paths produced by this task. If any are missing the task re-runs.
    fn outputs(&self) -> Vec<PathBuf>;

    /// Execute the task. Called only when inputs changed or outputs are missing.
    fn run(&self) -> Result<(), BuildError>;
}

#[derive(Debug)]
pub struct BuildError(pub String);

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

pub fn build_err(msg: impl Into<String>) -> BuildError {
    BuildError(msg.into())
}
