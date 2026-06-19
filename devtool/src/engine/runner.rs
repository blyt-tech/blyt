use std::path::Path;

use crate::engine::fingerprint::{
    TaskState, check_inputs, load_task_state, outputs_exist, save_task_state,
};
use crate::engine::{BuildError, Task, TaskInput};

/// Run an ordered list of tasks, skipping any whose inputs are up-to-date.
///
/// Tasks are executed sequentially. Execution stops immediately on the first
/// failure. When `force` is true all fingerprint checks are bypassed and every
/// task runs unconditionally.
///
/// `state_dir` is the directory under which per-task state files are stored
/// (typically `build/.blyt-tasks/<variant>/`).
pub fn run_tasks(tasks: &[Box<dyn Task>], state_dir: &Path, force: bool) -> Result<(), BuildError> {
    // The blyt executable is an implicit input to every task so that rebuilding
    // blyt itself invalidates all cached task state — same principle as tracking
    // compiler paths. Resolved once per run_tasks call.
    let blyt_exe = std::env::current_exe().ok();

    let effective_inputs = |task: &dyn Task| -> Vec<TaskInput> {
        let mut v = task.inputs();
        if let Some(ref exe) = blyt_exe {
            v.push(TaskInput::File(exe.clone()));
        }
        v
    };

    for task in tasks {
        let state_file = state_dir.join(format!("{}.state", task.key()));
        let outputs = task.outputs();

        if !force {
            let outputs_ok = outputs_exist(&outputs);
            if outputs_ok {
                let prev = load_task_state(&state_file);
                let (new_inputs, needs_run) =
                    check_inputs(&effective_inputs(task.as_ref()), prev.as_ref());
                if !needs_run {
                    // Up-to-date: persist any mtime refreshes from the fast path.
                    let updated = TaskState {
                        inputs: new_inputs,
                        outputs: outputs
                            .iter()
                            .map(|p| p.to_string_lossy().into_owned())
                            .collect(),
                    };
                    let _ = save_task_state(&state_file, &updated);
                    continue;
                }
            }
        }

        println!("  {}", task.label());
        task.run()?;

        // Record the post-run fingerprint so the next build can skip this task.
        let (new_inputs, _) = check_inputs(&effective_inputs(task.as_ref()), None);
        let state = TaskState {
            inputs: new_inputs,
            outputs: outputs
                .iter()
                .map(|p| p.to_string_lossy().into_owned())
                .collect(),
        };
        if let Err(e) = save_task_state(&state_file, &state) {
            eprintln!(
                "blyt: warning: could not save build state for {}: {e}",
                task.key()
            );
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::engine::{TaskInput, build_err};
    use std::fs;
    use std::path::PathBuf;
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    };
    use tempfile::TempDir;

    struct FakeTask {
        key_str: String,
        input_files: Vec<PathBuf>,
        input_values: Vec<String>,
        output_files: Vec<PathBuf>,
        run_count: Arc<AtomicUsize>,
        fail: bool,
    }

    impl FakeTask {
        fn new(key: &str, run_count: Arc<AtomicUsize>) -> Self {
            FakeTask {
                key_str: key.to_string(),
                input_files: vec![],
                input_values: vec![],
                output_files: vec![],
                run_count,
                fail: false,
            }
        }

        fn with_input_file(mut self, p: PathBuf) -> Self {
            self.input_files.push(p);
            self
        }

        fn with_input_value(mut self, v: &str) -> Self {
            self.input_values.push(v.to_string());
            self
        }

        fn with_output(mut self, p: PathBuf) -> Self {
            self.output_files.push(p);
            self
        }

        fn failing(mut self) -> Self {
            self.fail = true;
            self
        }
    }

    impl Task for FakeTask {
        fn key(&self) -> &str {
            &self.key_str
        }

        fn label(&self) -> String {
            self.key_str.clone()
        }

        fn inputs(&self) -> Vec<TaskInput> {
            let mut v: Vec<TaskInput> = self
                .input_files
                .iter()
                .map(|p| TaskInput::File(p.clone()))
                .collect();
            for val in &self.input_values {
                v.push(TaskInput::Value(val.clone()));
            }
            v
        }

        fn outputs(&self) -> Vec<PathBuf> {
            self.output_files.clone()
        }

        fn run(&self) -> Result<(), BuildError> {
            self.run_count.fetch_add(1, Ordering::SeqCst);
            if self.fail {
                return Err(build_err("injected failure"));
            }
            // Write outputs so subsequent runs see them as present.
            for out in &self.output_files {
                if let Some(parent) = out.parent() {
                    fs::create_dir_all(parent).ok();
                }
                fs::write(out, b"output").ok();
            }
            Ok(())
        }
    }

    fn boxed(t: FakeTask) -> Box<dyn Task> {
        Box::new(t)
    }

    // --- helpers ---

    fn write_file(path: &Path, content: &[u8]) {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).unwrap();
        }
        fs::write(path, content).unwrap();
    }

    // --- tests ---

    #[test]
    fn first_run_executes_task() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");
        // An absent output ensures the task is not vacuously skipped.
        let out = dir.path().join("out.o");
        let count = Arc::new(AtomicUsize::new(0));
        let tasks = vec![boxed(FakeTask::new("t", count.clone()).with_output(out))];
        run_tasks(&tasks, &state_dir, false).unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn second_run_skips_up_to_date_task() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");
        let src = dir.path().join("src.c");
        write_file(&src, b"int x;");
        let out = dir.path().join("out.o");

        let count = Arc::new(AtomicUsize::new(0));
        let make = || {
            vec![boxed(
                FakeTask::new("t", count.clone())
                    .with_input_file(src.clone())
                    .with_output(out.clone()),
            )]
        };

        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 1);

        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(
            count.load(Ordering::SeqCst),
            1,
            "should be skipped on second run"
        );
    }

    #[test]
    fn reruns_when_input_file_changes() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");
        let src = dir.path().join("src.c");
        write_file(&src, b"int x;");
        let out = dir.path().join("out.o");

        let count = Arc::new(AtomicUsize::new(0));
        let make = || {
            vec![boxed(
                FakeTask::new("t", count.clone())
                    .with_input_file(src.clone())
                    .with_output(out.clone()),
            )]
        };

        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 1);

        // Write content of a different length so the size check forces the slow
        // path regardless of mtime granularity on the test filesystem.
        write_file(&src, b"int y = 42; /* changed */");
        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(
            count.load(Ordering::SeqCst),
            2,
            "input changed → should rerun"
        );
    }

    #[test]
    fn reruns_when_output_missing() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");
        let out = dir.path().join("out.o");

        let count = Arc::new(AtomicUsize::new(0));
        let make = || {
            vec![boxed(
                FakeTask::new("t", count.clone()).with_output(out.clone()),
            )]
        };

        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 1);

        fs::remove_file(&out).unwrap();
        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(
            count.load(Ordering::SeqCst),
            2,
            "output deleted → should rerun"
        );
    }

    #[test]
    fn force_reruns_all_tasks() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");
        let src = dir.path().join("src.c");
        write_file(&src, b"int x;");
        let out = dir.path().join("out.o");

        let count = Arc::new(AtomicUsize::new(0));
        let make = || {
            vec![boxed(
                FakeTask::new("t", count.clone())
                    .with_input_file(src.clone())
                    .with_output(out.clone()),
            )]
        };

        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 1);

        // force=true — must rerun even though nothing changed.
        run_tasks(&make(), &state_dir, true).unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 2, "force flag should rerun");
    }

    #[test]
    fn stops_on_first_failure() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");

        let count_a = Arc::new(AtomicUsize::new(0));
        let count_b = Arc::new(AtomicUsize::new(0));
        // Both tasks declare absent outputs so they won't be vacuously skipped.
        let out_a = dir.path().join("a.o");
        let out_b = dir.path().join("b.o");
        let tasks: Vec<Box<dyn Task>> = vec![
            boxed(
                FakeTask::new("a", count_a.clone())
                    .with_output(out_a)
                    .failing(),
            ),
            boxed(FakeTask::new("b", count_b.clone()).with_output(out_b)),
        ];

        let result = run_tasks(&tasks, &state_dir, false);
        assert!(result.is_err());
        assert_eq!(count_a.load(Ordering::SeqCst), 1);
        assert_eq!(
            count_b.load(Ordering::SeqCst),
            0,
            "second task should not run after failure"
        );
    }

    #[test]
    fn value_input_change_triggers_rerun() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");

        let count = Arc::new(AtomicUsize::new(0));

        // First run with value "v1".
        run_tasks(
            &[boxed(
                FakeTask::new("t", count.clone()).with_input_value("v1"),
            )],
            &state_dir,
            false,
        )
        .unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 1);

        // Second run, same value — should skip.
        run_tasks(
            &[boxed(
                FakeTask::new("t", count.clone()).with_input_value("v1"),
            )],
            &state_dir,
            false,
        )
        .unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 1, "same value → skip");

        // Third run with different value — must rerun.
        run_tasks(
            &[boxed(
                FakeTask::new("t", count.clone()).with_input_value("v2"),
            )],
            &state_dir,
            false,
        )
        .unwrap();
        assert_eq!(count.load(Ordering::SeqCst), 2, "changed value → rerun");
    }

    #[test]
    fn state_persists_across_independent_runner_calls() {
        let dir = TempDir::new().unwrap();
        let state_dir = dir.path().join("state");
        let src = dir.path().join("src.c");
        write_file(&src, b"int x;");
        let out = dir.path().join("out.o");

        let count = Arc::new(AtomicUsize::new(0));
        let make = || {
            vec![boxed(
                FakeTask::new("t", count.clone())
                    .with_input_file(src.clone())
                    .with_output(out.clone()),
            )]
        };

        // Simulate two separate process invocations (different run_tasks calls).
        run_tasks(&make(), &state_dir, false).unwrap();
        run_tasks(&make(), &state_dir, false).unwrap();
        assert_eq!(
            count.load(Ordering::SeqCst),
            1,
            "state file should persist skip across calls"
        );
    }
}
