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
    /// Modification time in **nanoseconds** since the Unix epoch (see
    /// `file_meta`).  Whole-second resolution here would let a same-size edit
    /// within one second slip through the fast path unrebuilt (issue #290).
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
    // Nanosecond mtime, not whole seconds.  The size+mtime fast path below waves
    // a file through as "unchanged" when both match the recorded state — so a
    // byte-length-identical edit (e.g. a hot-reload source save changing `1` to
    // `2`) that lands in the *same wall-clock second* as the previous build
    // collides on a seconds-resolution mtime and is silently skipped, never
    // rebuilt or reloaded (issue #290).  Sub-second precision distinguishes
    // those writes on every filesystem we target (ext4/APFS/overlayfs all keep
    // nanosecond mtimes); on the rare 1 s-resolution filesystem the sub-second
    // part is 0 and we degrade to exactly the old behaviour — never worse.  A
    // wider mtime only ever steers more edits onto the hash slow path, which is
    // always the safe direction.  (u128 ns since the epoch fits u64 for ~500
    // years.)
    let mtime = meta
        .modified()
        .ok()?
        .duration_since(UNIX_EPOCH)
        .ok()?
        .as_nanos() as u64;
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::TaskInput;
    use tempfile::TempDir;

    fn write_file(dir: &TempDir, name: &str, content: &[u8]) -> PathBuf {
        let path = dir.path().join(name);
        fs::write(&path, content).unwrap();
        path
    }

    // --- parse_depfile ---

    #[test]
    fn parse_depfile_missing_returns_empty() {
        let dir = TempDir::new().unwrap();
        let result = parse_depfile(&dir.path().join("missing.d"));
        assert!(result.is_empty());
    }

    #[test]
    fn parse_depfile_no_colon_returns_empty() {
        let dir = TempDir::new().unwrap();
        let path = write_file(&dir, "no_colon.d", b"foo.o foo.c foo.h\n");
        assert!(parse_depfile(&path).is_empty());
    }

    #[test]
    fn parse_depfile_single_line() {
        let dir = TempDir::new().unwrap();
        let path = write_file(&dir, "single.d", b"foo.o: foo.c include/foo.h\n");
        let deps = parse_depfile(&path);
        assert_eq!(
            deps,
            vec![PathBuf::from("foo.c"), PathBuf::from("include/foo.h")]
        );
    }

    #[test]
    fn parse_depfile_continuation_lines() {
        let dir = TempDir::new().unwrap();
        let content = b"foo.o: foo.c \\\n  include/a.h \\\n  include/b.h\n";
        let path = write_file(&dir, "multi.d", content);
        let deps = parse_depfile(&path);
        assert_eq!(
            deps,
            vec![
                PathBuf::from("foo.c"),
                PathBuf::from("include/a.h"),
                PathBuf::from("include/b.h"),
            ]
        );
    }

    // --- outputs_exist ---

    #[test]
    fn outputs_exist_empty_slice_is_true() {
        assert!(outputs_exist(&[]));
    }

    #[test]
    fn outputs_exist_all_present() {
        let dir = TempDir::new().unwrap();
        let a = write_file(&dir, "a.o", b"x");
        let b = write_file(&dir, "b.o", b"y");
        assert!(outputs_exist(&[a, b]));
    }

    #[test]
    fn outputs_exist_one_missing() {
        let dir = TempDir::new().unwrap();
        let a = write_file(&dir, "a.o", b"x");
        let missing = dir.path().join("missing.o");
        assert!(!outputs_exist(&[a, missing]));
    }

    // --- check_inputs: File inputs ---

    #[test]
    fn check_inputs_new_file_needs_run() {
        let dir = TempDir::new().unwrap();
        let path = write_file(&dir, "src.c", b"int main() {}");
        let inputs = vec![TaskInput::File(path)];
        let (map, needs_run) = check_inputs(&inputs, None);
        assert!(needs_run);
        assert_eq!(map.len(), 1);
    }

    #[test]
    fn check_inputs_fast_path_unchanged() {
        let dir = TempDir::new().unwrap();
        let path = write_file(&dir, "src.c", b"int main() {}");

        // First pass — establish state.
        let (map, _) = check_inputs(&[TaskInput::File(path.clone())], None);
        let prev = TaskState {
            inputs: map,
            outputs: vec![],
        };

        // Second pass with same mtime — should take fast path, no run needed.
        let (_, needs_run) = check_inputs(&[TaskInput::File(path)], Some(&prev));
        assert!(!needs_run);
    }

    #[test]
    fn check_inputs_slow_path_content_unchanged() {
        let dir = TempDir::new().unwrap();
        let content = b"int main() {}";
        let path = write_file(&dir, "src.c", content);

        let (map, _) = check_inputs(&[TaskInput::File(path.clone())], None);
        let key = path.to_string_lossy().into_owned();
        let real_fs = map[&key].clone();

        // Simulate a touched mtime (force slow path) but identical content.
        let stale = FileState {
            mtime: real_fs.mtime.saturating_sub(1),
            ..real_fs.clone()
        };
        let prev = TaskState {
            inputs: [(key, stale)].into_iter().collect(),
            outputs: vec![],
        };

        let (_, needs_run) = check_inputs(&[TaskInput::File(path)], Some(&prev));
        assert!(
            !needs_run,
            "content is the same, slow path should detect no change"
        );
    }

    #[test]
    fn check_inputs_slow_path_content_changed() {
        let dir = TempDir::new().unwrap();
        let path = write_file(&dir, "src.c", b"int main() {}");

        let (map, _) = check_inputs(&[TaskInput::File(path.clone())], None);
        let key = path.to_string_lossy().into_owned();
        let real_fs = map[&key].clone();

        // Same size and stale mtime but a different hash — simulates content change
        // discovered only by hashing.
        let stale = FileState {
            mtime: real_fs.mtime.saturating_sub(1),
            hash: real_fs.hash ^ 0xdead_beef,
            ..real_fs
        };
        let prev = TaskState {
            inputs: [(key, stale)].into_iter().collect(),
            outputs: vec![],
        };

        let (_, needs_run) = check_inputs(&[TaskInput::File(path)], Some(&prev));
        assert!(needs_run, "hash mismatch should trigger rerun");
    }

    #[test]
    fn same_second_same_size_edit_is_detected() {
        // Regression for #290.  A byte-length-identical edit that lands in the
        // SAME wall-clock second as the previous build must still rerun the
        // task.  We pin the edited file's mtime to exactly one nanosecond after
        // the baseline's — same whole second — so a seconds-resolution mtime
        // would collide and the size+mtime fast path would wave the edit through
        // as "unchanged", silently skipping the rebuild (and the hot reload).
        // Sub-second mtime instead sends it to the hash slow path, which sees the
        // content change.  Forcing the mtime keeps this deterministic regardless
        // of how fast the test ran or the filesystem's mtime granularity.
        use std::time::{Duration, UNIX_EPOCH};

        let dir = TempDir::new().unwrap();
        let path = write_file(&dir, "src.lua", b"local _ = 1");

        let (map, _) = check_inputs(&[TaskInput::File(path.clone())], None);
        let key = path.to_string_lossy().into_owned();
        let baseline_mtime = map[&key].mtime; // nanoseconds since the epoch
        let prev = TaskState {
            inputs: map,
            outputs: vec![],
        };

        // Same size, different content, pinned to baseline + 1 ns (same second).
        fs::write(&path, b"local _ = 2").unwrap();
        let forced = UNIX_EPOCH + Duration::from_nanos(baseline_mtime + 1);
        let f = fs::OpenOptions::new().write(true).open(&path).unwrap();
        f.set_modified(forced).unwrap();
        drop(f);

        let (_, needs_run) = check_inputs(&[TaskInput::File(path)], Some(&prev));
        assert!(
            needs_run,
            "same-second, same-size content edit must be detected and rerun (#290)"
        );
    }

    #[test]
    fn check_inputs_missing_file_needs_run() {
        let dir = TempDir::new().unwrap();
        let missing = dir.path().join("gone.c");
        let (map, needs_run) = check_inputs(&[TaskInput::File(missing)], None);
        assert!(needs_run);
        assert!(
            map.is_empty(),
            "missing file should not appear in new state"
        );
    }

    // --- check_inputs: Value inputs ---

    #[test]
    fn check_inputs_new_value_needs_run() {
        let inputs = vec![TaskInput::Value("v1".to_string())];
        let (map, needs_run) = check_inputs(&inputs, None);
        assert!(needs_run);
        assert!(map.contains_key("value:v1"));
    }

    #[test]
    fn check_inputs_same_value_no_run() {
        let inputs = vec![TaskInput::Value("v1".to_string())];
        let (map, _) = check_inputs(&inputs, None);
        let prev = TaskState {
            inputs: map,
            outputs: vec![],
        };
        let (_, needs_run) = check_inputs(&inputs, Some(&prev));
        assert!(!needs_run);
    }

    #[test]
    fn check_inputs_changed_value_needs_run() {
        let inputs_v1 = vec![TaskInput::Value("v1".to_string())];
        let (map, _) = check_inputs(&inputs_v1, None);
        let prev = TaskState {
            inputs: map,
            outputs: vec![],
        };

        let inputs_v2 = vec![TaskInput::Value("v2".to_string())];
        let (_, needs_run) = check_inputs(&inputs_v2, Some(&prev));
        assert!(needs_run);
    }

    // --- save/load roundtrip ---

    #[test]
    fn save_load_roundtrip() {
        let dir = TempDir::new().unwrap();
        let state_file = dir.path().join("task.state");
        let state = TaskState {
            inputs: [(
                "value:foo".to_string(),
                FileState {
                    size: 0,
                    mtime: 0,
                    hash: 42,
                },
            )]
            .into_iter()
            .collect(),
            outputs: vec!["out/foo.o".to_string()],
        };
        save_task_state(&state_file, &state).unwrap();
        let loaded = load_task_state(&state_file).unwrap();
        assert_eq!(loaded.outputs, state.outputs);
        assert_eq!(loaded.inputs["value:foo"].hash, 42);
    }

    #[test]
    fn load_task_state_missing_returns_none() {
        let dir = TempDir::new().unwrap();
        assert!(load_task_state(&dir.path().join("no.state")).is_none());
    }

    #[test]
    fn load_task_state_corrupt_returns_none() {
        let dir = TempDir::new().unwrap();
        let path = write_file(&dir, "bad.state", b"not json {{{{");
        assert!(load_task_state(&path).is_none());
    }
}
