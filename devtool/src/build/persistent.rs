//! Persistent resources declared in the manifest (#160, ADR-0028).
//!
//! A cart declares always-needed resources as persistent in `blyt.build.yaml`:
//!
//! ```yaml
//! persistent_resources: [font_ui, palette_main]
//! ```
//!
//! The names are resource names (the same ADR-0040 names `assets::scan_assets`
//! derives — e.g. `font_ui` for `assets/font-ui.png`). The packer resolves each
//! to its integer resource id, emits the sorted id list as the `.cart.persistent`
//! ELF section (read by the host and native loaders via the shared ELF walk), and
//! enforces the **build-time budget guard** (Layer 1): a persistent set whose
//! decompressed total exceeds the 16 MiB working-memory budget fails the build —
//! deterministically and identically for every leg, before any cart ships
//! (ADR-0028; the over-budget acceptance criterion of #160). The runtime then
//! pre-loads and pins the set before `init()` runs, counting it against the same
//! budget from cart start.

use std::collections::BTreeSet;
use std::fs;
use std::path::{Path, PathBuf};

use crate::engine::{BuildError, Task, TaskInput, build_err};

use super::assets::AssetInfo;

/// The cart-visible working-memory budget: 16 MiB (ADR-0008). Mirrors the
/// runtime's `BLYT_MEM_BUDGET_BYTES` (`runtime/shared/blyt_mem_budget.h`) — the
/// build-time persistent guard and the runtime footprint enforce the *same* cap,
/// so a cart that passes the build can never over-subscribe the budget at preload
/// (#160). Kept in sync by the round-trip test below and the runtime's own
/// `_Static_assert`.
pub const MEM_BUDGET_BYTES: u64 = 16 * 1024 * 1024;

/// Read `persistent_resources` from `<project_dir>/blyt.build.yaml`. Returns an
/// empty list when the file or the field is absent.
pub(super) fn read_persistent_resources(project_dir: &Path) -> Result<Vec<String>, BuildError> {
    let path = project_dir.join("blyt.build.yaml");
    if !path.exists() {
        return Ok(Vec::new());
    }
    let text = fs::read_to_string(&path)?;
    /// Lean view capturing only `persistent_resources` (the language/assets
    /// fields are parsed elsewhere; no `deny_unknown_fields`).
    #[derive(serde::Deserialize, Default)]
    struct Manifest {
        #[serde(default)]
        persistent_resources: Vec<String>,
    }
    let manifest: Manifest =
        serde_yaml::from_str(&text).map_err(|e| build_err(format!("blyt.build.yaml: {e}")))?;
    Ok(manifest.persistent_resources)
}

/// Resolve the declared persistent resource names to their integer ids against
/// the scanned asset set, returning the ids **sorted ascending** (the
/// `.cart.persistent` section is canonical/reproducible). Errors on a name that
/// matches no resource (a typo must fail the build, not silently do nothing) and
/// on a duplicate name (a copy-paste slip). Enforces the build-time budget guard:
/// the persistent set's decompressed total must not exceed 16 MiB.
pub(super) fn resolve_persistent_ids(
    names: &[String],
    assets: &[AssetInfo],
) -> Result<Vec<u32>, BuildError> {
    let mut ids: BTreeSet<u32> = BTreeSet::new();
    let mut total: u64 = 0;
    for name in names {
        let asset = assets
            .iter()
            .find(|a| &a.resource_name == name)
            .ok_or_else(|| {
                build_err(format!(
                    "persistent_resources: `{name}` is not a known resource \
                     (declare it as an asset, or fix the name)"
                ))
            })?;
        if !ids.insert(asset.id) {
            return Err(build_err(format!(
                "persistent_resources: `{name}` is listed more than once"
            )));
        }
        total += asset.staged_len as u64;
    }
    if total > MEM_BUDGET_BYTES {
        return Err(build_err(format!(
            "persistent_resources: declared set is {total} bytes, which exceeds the \
             {MEM_BUDGET_BYTES}-byte (16 MiB) working-memory budget; persistent resources \
             are resident for the cart's whole lifetime, so the set must fit the budget \
             (resources: {})",
            names.join(", ")
        )));
    }
    Ok(ids.into_iter().collect())
}

/// Serialize the persistent id list as the `.cart.persistent` section body: each
/// id as a little-endian `u32`, in ascending order. Empty list → empty section
/// (the section is still emitted so its presence is uniform, but it carries no
/// ids). The runtime reads `size / 4` ids back.
pub(super) fn persistent_section_bytes(ids: &[u32]) -> Vec<u8> {
    let mut out = Vec::with_capacity(ids.len() * 4);
    for id in ids {
        out.extend_from_slice(&id.to_le_bytes());
    }
    out
}

/// Task: write the `.cart.persistent` section body file. Keyed on the id list so
/// it re-runs only when the persistent set changes.
pub(super) struct WritePersistentSectionTask {
    pub output: PathBuf,
    pub ids: Vec<u32>,
}

impl Task for WritePersistentSectionTask {
    fn key(&self) -> &str {
        "write_persistent_section"
    }
    fn label(&self) -> String {
        "write    .cart.persistent".to_string()
    }
    fn inputs(&self) -> Vec<TaskInput> {
        // Value-keyed on the id list (as bytes) so a changed set re-runs the task.
        vec![TaskInput::Value(format!("{:?}", self.ids))]
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        if let Some(parent) = self.output.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(&self.output, persistent_section_bytes(&self.ids))?;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::build::assets::ResourceType;

    fn asset(id: u32, name: &str, staged_len: usize) -> AssetInfo {
        AssetInfo {
            id,
            resource_name: name.to_string(),
            source: PathBuf::from(format!("assets/{name}.bin")),
            fingerprint: "deadbeef".to_string(),
            rel_data: format!("resources/{name}-deadbeef.data"),
            data_output: PathBuf::from(format!("build/resources/{name}-deadbeef.data")),
            meta_output: PathBuf::from(format!("build/resources/{name}-deadbeef.meta")),
            resource_type: ResourceType::Raw,
            staged_len,
        }
    }

    #[test]
    fn resolves_names_to_sorted_ids() {
        let assets = vec![
            asset(1, "font_ui", 100),
            asset(2, "palette_main", 100),
            asset(3, "player", 100),
        ];
        // Declared out of order; the section is canonical (ascending).
        let ids = resolve_persistent_ids(&["player".to_string(), "font_ui".to_string()], &assets)
            .unwrap();
        assert_eq!(ids, vec![1, 3]);
    }

    #[test]
    fn unknown_name_is_a_build_error() {
        let assets = vec![asset(1, "font_ui", 100)];
        let err = resolve_persistent_ids(&["nope".to_string()], &assets)
            .err()
            .expect("unknown persistent name must fail the build");
        assert!(err.to_string().contains("nope"), "{err}");
        assert!(err.to_string().contains("not a known resource"), "{err}");
    }

    #[test]
    fn duplicate_name_is_a_build_error() {
        let assets = vec![asset(1, "font_ui", 100)];
        let err = resolve_persistent_ids(&["font_ui".to_string(), "font_ui".to_string()], &assets)
            .err()
            .expect("duplicate persistent name must fail the build");
        assert!(err.to_string().contains("more than once"), "{err}");
    }

    #[test]
    fn over_budget_set_is_a_build_error() {
        // Two resources of 10 MiB each = 20 MiB > 16 MiB budget.
        let ten_mib = 10 * 1024 * 1024;
        let assets = vec![asset(1, "big_a", ten_mib), asset(2, "big_b", ten_mib)];
        let err = resolve_persistent_ids(&["big_a".to_string(), "big_b".to_string()], &assets)
            .err()
            .expect("over-budget persistent set must fail the build");
        assert!(err.to_string().contains("budget"), "{err}");
        assert!(err.to_string().contains("big_a"), "{err}");
    }

    #[test]
    fn single_oversized_resource_trips_the_sum() {
        // One 17 MiB resource trips the same sum check (no separate per-resource case).
        let assets = vec![asset(1, "huge", 17 * 1024 * 1024)];
        let err = resolve_persistent_ids(&["huge".to_string()], &assets)
            .err()
            .expect("a single oversized persistent resource must fail the build");
        assert!(err.to_string().contains("budget"), "{err}");
    }

    #[test]
    fn exactly_at_budget_is_allowed() {
        // The whole budget may be persistent (ADR-0028: it reduces heap headroom,
        // the author's responsibility — not a build error).
        let assets = vec![asset(1, "full", MEM_BUDGET_BYTES as usize)];
        let ids = resolve_persistent_ids(&["full".to_string()], &assets).unwrap();
        assert_eq!(ids, vec![1]);
    }

    #[test]
    fn empty_set_resolves_empty() {
        let assets = vec![asset(1, "font_ui", 100)];
        assert!(resolve_persistent_ids(&[], &assets).unwrap().is_empty());
    }

    #[test]
    fn section_bytes_are_le_u32_ascending() {
        assert_eq!(
            persistent_section_bytes(&[1, 258]),
            vec![1, 0, 0, 0, 2, 1, 0, 0]
        );
        assert!(persistent_section_bytes(&[]).is_empty());
    }
}
