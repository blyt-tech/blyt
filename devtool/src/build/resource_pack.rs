//! Per-resource zstd compression for the packed cart (#157, ADR-0026).
//!
//! Each `.cart.resource.<id>` body section in a packed `.blyt` is encoded as an
//! 8-byte always-uncompressed header followed by the resource body, which is
//! either the raw bytes (`algo = none`) or a zstd frame (`algo = zstd`). The
//! packer decides per-resource: it keeps the zstd frame only when it saves more
//! than 5% of the resource size, so already-compressed content (and
//! tiny/incompressible resources) ship `none` and never grow from a wasted
//! compression attempt. Authors never configure this (ADR-0026).
//!
//! Header layout (little-endian):
//!   off 0  u8    algo      (0 = none, 1 = zstd)
//!   off 1  u8×3  reserved  (zero)
//!   off 4  u32   dsize     decompressed (original) body length in bytes
//!   off 8  ..    body      raw bytes or a zstd frame, `len = section - 8`
//!
//! The header is the per-resource entry of ADR-0026's "always-uncompressed
//! metadata/index": the runtime reads `algo`/`dsize` without touching the body,
//! so it can serve an uncompressed resource zero-copy (`data = section + 8`)
//! and, for the rest of the epic (#156), size a decompress buffer / decide
//! eviction before allocating. Decode is the C runtime's job (host + native);
//! this is the sole canonical encoder, so the on-disk format is defined here.

/// Size of the always-uncompressed per-resource header prefixed to every
/// `.cart.resource.<id>` section.
pub const RES_HEADER_LEN: usize = 8;

/// `algo` byte values.
pub const RES_ALGO_NONE: u8 = 0;
pub const RES_ALGO_ZSTD: u8 = 1;

/// Fixed zstd compression level (ADR-0026 specifies "medium"). Pinned so the
/// packer's output is reproducible — a given input + zstd version must always
/// produce byte-identical cart bytes (acceptance criterion). Carts are small, so
/// encode speed is irrelevant; we favour ratio.
pub const RES_ZSTD_LEVEL: i32 = 19;

use std::fs;
use std::path::PathBuf;

use crate::engine::{BuildError, Task, TaskInput};

/// Phase-2 task: compress one staged resource `.data` into its packed on-disk
/// `.res` blob (header + body) for embedding as a `.cart.resource.<id>` section.
/// Runs only for the packed `.blyt` — dev mode serves the staging `.data`
/// uncompressed, so this never touches the dev path. Re-runs when the staged
/// bytes change (the engine keys on the input file's fingerprint).
pub(super) struct PackResourceTask {
    pub resource_name: String,
    /// Staged `.data` (text: content + trailing NUL; raw: byte-exact) — input.
    pub staged_data: PathBuf,
    /// Packed `.res` blob (8-byte header + raw|zstd body) — output / section src.
    pub packed_output: PathBuf,
    pub key_str: String,
}

impl Task for PackResourceTask {
    fn key(&self) -> &str {
        &self.key_str
    }
    fn label(&self) -> String {
        format!("pack     {}", self.resource_name)
    }
    fn inputs(&self) -> Vec<TaskInput> {
        vec![TaskInput::File(self.staged_data.clone())]
    }
    fn outputs(&self) -> Vec<PathBuf> {
        vec![self.packed_output.clone()]
    }
    fn run(&self) -> Result<(), BuildError> {
        let raw = fs::read(&self.staged_data)?;
        let encoded = encode_resource(&raw);
        if let Some(parent) = self.packed_output.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(&self.packed_output, &encoded)?;
        Ok(())
    }
}

/// Encode one resource body into its packed on-disk form (8-byte header + body).
/// Compresses with zstd only when that saves more than 5% of the resource size;
/// otherwise stores the bytes verbatim under `algo = none` (so an incompressible
/// resource never grows from a wasted zstd frame — only the constant header).
pub fn encode_resource(raw: &[u8]) -> Vec<u8> {
    let dsize = raw.len();

    // Try zstd, but keep the frame only if it saves > 5% of the raw size:
    //   savings > 0.05 * dsize  <=>  compressed < 0.95 * dsize
    // (integer form avoids float rounding). Empty input is never compressed.
    let compressed = if raw.is_empty() {
        None
    } else {
        zstd::bulk::compress(raw, RES_ZSTD_LEVEL).ok()
    };
    let use_zstd = matches!(&compressed, Some(c) if (c.len() as u64) * 100 < (dsize as u64) * 95);

    let (algo, body): (u8, &[u8]) = if use_zstd {
        (RES_ALGO_ZSTD, compressed.as_deref().unwrap())
    } else {
        (RES_ALGO_NONE, raw)
    };

    let mut out = Vec::with_capacity(RES_HEADER_LEN + body.len());
    out.push(algo);
    out.extend_from_slice(&[0, 0, 0]); // reserved
    out.extend_from_slice(&(dsize as u32).to_le_bytes());
    out.extend_from_slice(body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The runtime's decode, mirrored in Rust for round-trip assertions: parse
    /// the header, then return the body verbatim (none) or zstd-decompressed to
    /// exactly `dsize` bytes (zstd).
    fn decode_resource(section: &[u8]) -> Vec<u8> {
        assert!(
            section.len() >= RES_HEADER_LEN,
            "section shorter than header"
        );
        let algo = section[0];
        let dsize = u32::from_le_bytes(section[4..8].try_into().unwrap()) as usize;
        let body = &section[RES_HEADER_LEN..];
        match algo {
            RES_ALGO_NONE => {
                assert_eq!(body.len(), dsize, "none body length must equal dsize");
                body.to_vec()
            }
            RES_ALGO_ZSTD => {
                let out = zstd::bulk::decompress(body, dsize).unwrap();
                assert_eq!(out.len(), dsize, "decompressed length must equal dsize");
                out
            }
            other => panic!("unknown algo {other}"),
        }
    }

    fn algo(section: &[u8]) -> u8 {
        section[0]
    }
    fn dsize(section: &[u8]) -> usize {
        u32::from_le_bytes(section[4..8].try_into().unwrap()) as usize
    }

    #[test]
    fn highly_compressible_text_packs_zstd_and_shrinks() {
        // A long run of repeated text compresses far below 95% of its size.
        let raw = "the quick brown fox jumps over the lazy dog. ".repeat(200);
        let raw = raw.as_bytes();
        let section = encode_resource(raw);
        assert_eq!(
            algo(&section),
            RES_ALGO_ZSTD,
            "compressible text must pack zstd"
        );
        // On-disk section (header + zstd frame) is smaller than the raw bytes.
        assert!(
            section.len() < raw.len(),
            "section {} not smaller than raw {}",
            section.len(),
            raw.len()
        );
        assert_eq!(dsize(&section), raw.len());
        assert_eq!(
            decode_resource(&section),
            raw,
            "round-trip must be byte-exact"
        );
    }

    /// Deterministic high-entropy bytes (splitmix64) — uniformly random, so zstd
    /// cannot meaningfully compress them. Used to exercise the `none` path.
    fn random_bytes(n: usize) -> Vec<u8> {
        let mut state: u64 = 0x9E37_79B9_7F4A_7C15;
        (0..n)
            .map(|_| {
                state = state.wrapping_add(0x9E37_79B9_7F4A_7C15);
                let mut z = state;
                z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
                z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
                (z ^ (z >> 31)) as u8
            })
            .collect()
    }

    #[test]
    fn incompressible_bytes_pack_none_without_body_growth() {
        // Uniformly random bytes: zstd would only grow them, so the packer must
        // store `none` with the body verbatim (no growth beyond the constant
        // header).
        let raw = random_bytes(4096);
        let section = encode_resource(&raw);
        assert_eq!(
            algo(&section),
            RES_ALGO_NONE,
            "incompressible data must pack none"
        );
        assert_eq!(dsize(&section), raw.len());
        assert_eq!(
            section.len() - RES_HEADER_LEN,
            raw.len(),
            "none body must be byte-exact (no compression growth)"
        );
        assert_eq!(decode_resource(&section), raw);
    }

    #[test]
    fn below_threshold_packs_none() {
        // Mostly-random bytes with a short compressible tail: zstd shrinks it a
        // little, but under the 5% threshold — so the packer must still ship
        // `none`, not a barely-smaller zstd frame.
        let mut raw = random_bytes(4000);
        raw.extend(std::iter::repeat_n(0u8, 128)); // ~3% best-case savings
        let zstd_len = zstd::bulk::compress(&raw, RES_ZSTD_LEVEL).unwrap().len();
        assert!(
            (zstd_len as u64) * 100 >= (raw.len() as u64) * 95,
            "fixture should be within 5% of raw (zstd={zstd_len}, raw={})",
            raw.len()
        );
        let section = encode_resource(&raw);
        assert_eq!(
            algo(&section),
            RES_ALGO_NONE,
            "below-threshold data must pack none"
        );
        assert_eq!(decode_resource(&section), raw);
    }

    #[test]
    fn empty_resource_packs_none() {
        let section = encode_resource(b"");
        assert_eq!(algo(&section), RES_ALGO_NONE);
        assert_eq!(dsize(&section), 0);
        assert_eq!(section.len(), RES_HEADER_LEN);
        assert_eq!(decode_resource(&section), b"");
    }

    #[test]
    fn encoding_is_reproducible() {
        // Same input -> byte-identical section (reproducible cart bytes).
        let raw = "lorem ipsum dolor sit amet ".repeat(100);
        assert_eq!(
            encode_resource(raw.as_bytes()),
            encode_resource(raw.as_bytes())
        );
    }

    #[test]
    fn header_reserved_bytes_are_zero() {
        let section = encode_resource(b"hello world");
        assert_eq!(&section[1..4], &[0, 0, 0]);
    }
}
