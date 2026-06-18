use std::path::Path;

use crate::engine::fingerprint::{
    TaskState, check_inputs, load_task_state, outputs_exist, save_task_state,
};
use crate::engine::{BuildError, Task};

/// Run an ordered list of tasks, skipping any whose inputs are up-to-date.
///
/// Tasks are executed sequentially. Execution stops immediately on the first
/// failure. When `force` is true all fingerprint checks are bypassed and every
/// task runs unconditionally.
///
/// `state_dir` is the directory under which per-task state files are stored
/// (typically `build/.blyt-tasks/<variant>/`).
pub fn run_tasks(
    tasks: &[Box<dyn Task>],
    state_dir: &Path,
    force: bool,
) -> Result<(), BuildError> {
    for task in tasks {
        let state_file = state_dir.join(format!("{}.state", task.key()));
        let outputs = task.outputs();

        if !force {
            let outputs_ok = outputs_exist(&outputs);
            if outputs_ok {
                let prev = load_task_state(&state_file);
                let (new_inputs, needs_run) = check_inputs(&task.inputs(), prev.as_ref());
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
        let (new_inputs, _) = check_inputs(&task.inputs(), None);
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
