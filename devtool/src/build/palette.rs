//! Palette-file parsers (#214, ADR-0088 amendment 2026-07-02): `.hex` (Lospec),
//! `.gpl` (GIMP), `.pal` (JASC-PAL text) -> a canonical 256-entry XRGB8888
//! table. Exact/integer conversion, no image decode.
//!
//! Source colors fill indices `0..N-1` verbatim (`N <= 256`; `> 256` is an
//! error); unspecified indices pad black (`0`); index 255 is **not** forced
//! (ADR-0049: index 255 holds a real color, transparent only at blit time — a
//! deferred image-tier concern). The output layout is XRGB8888 with the top
//! byte 0, matching `session->palette` / `blyt_builtin_palette`.
#![allow(dead_code)] // wired into the ResourceType::Palette transform in a later slice

use crate::engine::{BuildError, build_err};

pub(super) const PALETTE_ENTRIES: usize = 256;

/// Pack 8-bit `r,g,b` into XRGB8888 (top/alpha byte 0).
fn xrgb(r: u8, g: u8, b: u8) -> u32 {
    ((r as u32) << 16) | ((g as u32) << 8) | (b as u32)
}

/// Assemble a full 256-entry table from an ordered color list: colors fill
/// indices `0..N-1`, the remainder are black. `> 256` colors is a build error.
fn assemble(colors: Vec<u32>) -> Result<[u32; PALETTE_ENTRIES], BuildError> {
    if colors.len() > PALETTE_ENTRIES {
        return Err(build_err(format!(
            "palette has {} entries, exceeding the {PALETTE_ENTRIES}-color limit",
            colors.len()
        )));
    }
    let mut table = [0u32; PALETTE_ENTRIES];
    for (i, c) in colors.into_iter().enumerate() {
        table[i] = c;
    }
    Ok(table)
}

/// Parse a Lospec `.hex` palette: one `RRGGBB` (6 hex digits) per line, optional
/// leading `#`, blank/whitespace lines skipped, case-insensitive, RGB only.
pub(super) fn parse_hex(text: &str) -> Result<[u32; PALETTE_ENTRIES], BuildError> {
    let mut colors = Vec::new();
    for (i, raw) in text.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() {
            continue;
        }
        let hex = line.strip_prefix('#').unwrap_or(line);
        if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(build_err(format!(
                "palette .hex line {}: expected a 6-digit RRGGBB hex color, got {raw:?}",
                i + 1
            )));
        }
        // 6 hex digits are exactly the low 24 bits of XRGB8888 (top byte 0).
        colors.push(u32::from_str_radix(hex, 16).unwrap());
    }
    assemble(colors)
}

/// Parse the three decimal `R G B` components (0-255) at the start of a
/// whitespace-separated line; trailing tokens (e.g. a GIMP color label) are
/// ignored. `ctx` names the format + line for the error message.
fn parse_rgb_line(line: &str, ctx: &str) -> Result<u32, BuildError> {
    let mut it = line.split_whitespace();
    let mut next = |chan: &str| -> Result<u8, BuildError> {
        it.next()
            .ok_or_else(|| build_err(format!("{ctx}: missing {chan} component in {line:?}")))?
            .parse::<u8>()
            .map_err(|_| build_err(format!("{ctx}: {chan} component not a 0-255 integer in {line:?}")))
    };
    let (r, g, b) = (next("red")?, next("green")?, next("blue")?);
    Ok(xrgb(r, g, b))
}

/// Parse a GIMP `.gpl` palette: `GIMP Palette` magic first line; `#` comments,
/// blank lines, and `Key:` header lines (e.g. `Name:`, `Columns:`) skipped;
/// each color line is `R G B` decimals + an optional trailing label (ignored).
/// A line whose first character is a digit is a color line; malformed ones are
/// build errors with the line number.
pub(super) fn parse_gpl(text: &str) -> Result<[u32; PALETTE_ENTRIES], BuildError> {
    let mut seen_magic = false;
    let mut colors = Vec::new();
    for (i, raw) in text.lines().enumerate() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if !seen_magic {
            if line != "GIMP Palette" {
                return Err(build_err(
                    "palette .gpl: missing 'GIMP Palette' header on the first line".to_string(),
                ));
            }
            seen_magic = true;
            continue;
        }
        // A color line starts with a digit (the red component); `Key:` header
        // lines (Name:, Columns:) start with a letter -> skip.
        if !line.starts_with(|c: char| c.is_ascii_digit()) {
            continue;
        }
        colors.push(parse_rgb_line(line, &format!("palette .gpl line {}", i + 1))?);
    }
    if !seen_magic {
        return Err(build_err(
            "palette .gpl: missing 'GIMP Palette' header (empty file)".to_string(),
        ));
    }
    assemble(colors)
}

/// Parse a JASC-PAL `.pal` palette (text): `JASC-PAL` magic, `0100` version,
/// a decimal count, then that many `R G B` decimal lines.
pub(super) fn parse_pal(text: &str) -> Result<[u32; PALETTE_ENTRIES], BuildError> {
    let mut lines = text.lines().enumerate().map(|(i, l)| (i + 1, l.trim()));
    let (_, magic) = lines.next().unwrap_or((0, ""));
    if magic != "JASC-PAL" {
        return Err(build_err(
            "palette .pal: not a JASC-PAL file (missing 'JASC-PAL' header)".to_string(),
        ));
    }
    let (_, version) = lines.next().unwrap_or((0, ""));
    if version != "0100" {
        return Err(build_err(format!(
            "palette .pal: unsupported JASC-PAL version {version:?} (expected 0100)"
        )));
    }
    let (cn, count_str) = lines.next().unwrap_or((0, ""));
    let count: usize = count_str.parse().map_err(|_| {
        build_err(format!(
            "palette .pal line {cn}: entry count {count_str:?} is not an integer"
        ))
    })?;
    let mut colors = Vec::with_capacity(count.min(PALETTE_ENTRIES));
    for n in 0..count {
        let (ln, line) = lines.next().ok_or_else(|| {
            build_err(format!(
                "palette .pal: header declares {count} entries but only {n} color lines present"
            ))
        })?;
        colors.push(parse_rgb_line(line, &format!("palette .pal line {ln}"))?);
    }
    assemble(colors)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_parses_colors_in_order_padding_black() {
        let table = parse_hex("000000\nFF0000\n00ff00\n#0000FF\n").unwrap();
        assert_eq!(table[0], 0x00_00_00_00);
        assert_eq!(table[1], 0x00_FF_00_00);
        assert_eq!(table[2], 0x00_00_FF_00);
        assert_eq!(table[3], 0x00_00_00_FF); // leading '#' tolerated
        assert_eq!(table[4], 0); // unspecified -> black
        assert_eq!(table[255], 0);
    }

    #[test]
    fn hex_skips_blank_lines() {
        let table = parse_hex("\n  \nFF0000\n\n").unwrap();
        assert_eq!(table[0], 0x00_FF_00_00);
    }

    #[test]
    fn hex_rejects_malformed_line_with_line_number() {
        let err = parse_hex("FF0000\nnothex\n").unwrap_err().to_string();
        assert!(err.contains("line 2"), "got: {err}");
    }

    #[test]
    fn hex_rejects_more_than_256_colors() {
        let src = "FF0000\n".repeat(257);
        let err = parse_hex(&src).unwrap_err().to_string();
        assert!(err.contains("exceeding"), "got: {err}");
    }

    #[test]
    fn gpl_parses_colors_ignoring_headers_comments_labels() {
        let src = "GIMP Palette\nName: Test\nColumns: 4\n#\n0 0 0 Black\n255 0 0\t Red\n0 255 0\n";
        let table = parse_gpl(src).unwrap();
        assert_eq!(table[0], 0x00_00_00_00);
        assert_eq!(table[1], 0x00_FF_00_00);
        assert_eq!(table[2], 0x00_00_FF_00);
        assert_eq!(table[3], 0);
    }

    #[test]
    fn gpl_rejects_missing_magic() {
        let err = parse_gpl("Name: nope\n0 0 0\n").unwrap_err().to_string();
        assert!(err.to_lowercase().contains("gimp"), "got: {err}");
    }

    #[test]
    fn gpl_rejects_out_of_range_component() {
        let src = "GIMP Palette\n0 0 0\n300 0 0\n";
        let err = parse_gpl(src).unwrap_err().to_string();
        assert!(err.contains("line 3"), "got: {err}");
    }

    #[test]
    fn pal_parses_jasc_text() {
        let src = "JASC-PAL\n0100\n3\n0 0 0\n255 0 0\n0 0 255\n";
        let table = parse_pal(src).unwrap();
        assert_eq!(table[0], 0x00_00_00_00);
        assert_eq!(table[1], 0x00_FF_00_00);
        assert_eq!(table[2], 0x00_00_00_FF);
        assert_eq!(table[3], 0);
    }

    #[test]
    fn pal_rejects_missing_magic() {
        let err = parse_pal("0100\n1\n0 0 0\n").unwrap_err().to_string();
        assert!(err.to_lowercase().contains("jasc"), "got: {err}");
    }

    #[test]
    fn pal_rejects_count_exceeding_color_lines() {
        let src = "JASC-PAL\n0100\n3\n0 0 0\n255 0 0\n";
        let err = parse_pal(src).unwrap_err().to_string();
        assert!(err.contains("3") || err.to_lowercase().contains("count"), "got: {err}");
    }
}
