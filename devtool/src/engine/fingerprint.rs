use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::UNIX_EPOCH;

use serde::{Deserialize, Serialize};
use xxhash_rust::xxh3::xxh3_64;

use crate::engine::TaskInput;

#[derive(Serialize, Deserialize, Clone)]
pub struct FileState {
    pub size: u64,
    pub mtime: u64,
    pub hash: u64,
}

#[derive(Serialize, Deserialize)]
pub struct TaskState {
    pub inputs: HashMap<String, FileState>,
    pub outputs: Vec<String>,
}

pub fn load_task_state(state_file: &Path) -> Option<TaskState> {
    let text = fs::read_to_string(state_file).ok()?;
    serde_json::from_str(&text).ok()
}

pub fn save_task_state(state_file: &Path, state: &TaskState) -> std::io::Result<()> {
    if let Some(parent) = state_file.parent() {
        fs::create_dir_all(parent)?;
    }
    let json = serde_json::to_string_pretty(state)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e.to_string()))?;
    fs::write(state_file, json)
}

fn file_meta(path: &Path) -> Option<(u64, u64)> {
    let meta = fs::metadata(path).ok()?;
    let mtime = meta
        .modified()
        .ok()?
        .duration_since(UNIX_EPOCH)
        .ok()?
        .as_secs();
    Some((meta.len(), mtime))
}

fn hash_file(path: &Path) -> Option<u64> {
    fs::read(path).ok().map(|b| xxh3_64(&b))
}

/// Check a single file input. Returns (current FileState, changed).
/// If the file is absent, returns (None, true).
fn check_file(path: &Path, prev: Option<&FileState>) -> (Option<FileState>, bool) {
    let Some((size, mtime)) = file_meta(path) else {
        return (None, true);
    };
    match prev {
        Some(p) if p.size == size && p.mtime == mtime => {
            // Fast path: size and mtime match — assume unchanged.
            (Some(p.clone()), false)
        }
        Some(p) => {
            // Slow path: hash to confirm.
            let hash = match hash_file(path) {
                Some(h) => h,
                None => return (None, true),
            };
            let changed = p.hash != hash;
            (Some(FileState { size, mtime, hash }), changed)
        }
        None => {
            // New input — fingerprint it and treat as changed so the task runs once
            // to establish the baseline.
            let hash = hash_file(path).unwrap_or(0);
            (Some(FileState { size, mtime, hash }), true)
        }
    }
}

/// Check all task inputs against stored state.
/// Returns (new input map, needs_run).
pub fn check_inputs(
    inputs: &[TaskInput],
    prev: Option<&TaskState>,
) -> (HashMap<String, FileState>, bool) {
    let mut new_inputs: HashMap<String, FileState> = HashMap::new();
    let mut needs_run = false;

    for input in inputs {
        match input {
            TaskInput::File(path) => {
                let key = path.to_string_lossy().into_owned();
                let prev_fs = prev.and_then(|s| s.inputs.get(&key));
                let (fs, changed) = check_file(path, prev_fs);
                if changed {
                    needs_run = true;
                }
                if let Some(fs) = fs {
                    new_inputs.insert(key, fs);
                }
            }
            TaskInput::Value(val) => {
                // Key encodes the value itself — a changed value means a different key,
                // which won't be found in prev, triggering needs_run.
                let key = format!("value:{val}");
                if prev.and_then(|s| s.inputs.get(&key)).is_none() {
                    needs_run = true;
                }
                new_inputs.insert(
                    key,
                    FileState {
                        size: 0,
                        mtime: 0,
                        hash: xxh3_64(val.as_bytes()),
                    },
                );
            }
        }
    }

    (new_inputs, needs_run)
}

pub fn outputs_exist(outputs: &[PathBuf]) -> bool {
    outputs.iter().all(|p| p.exists())
}

/// Parse a Makefile depfile produced by `clang -MD -MF`.
/// Returns the list of dependency paths (excluding the primary target).
pub fn parse_depfile(path: &Path) -> Vec<PathBuf> {
    let Ok(text) = fs::read_to_string(path) else {
        return Vec::new();
    };
    let after_colon = match text.find(':') {
        Some(pos) => &text[pos + 1..],
        None => return Vec::new(),
    };
    after_colon
        .split(|c| c == '\n' || c == '\r')
        .flat_map(|line| {
            let line = line.trim().trim_end_matches('\\').trim();
            line.split_whitespace()
        })
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .collect()
}
