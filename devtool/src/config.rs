use std::collections::BTreeMap;
use std::fmt::Write as FmtWrite;
use std::path::Path;

/* -------------------------------------------------------------------------
 * blyt.config.yaml parsing (ADR-0009, ADR-0010)
 *
 * Optional manifest file for runtime configuration and state buffer
 * declarations. When absent, all fields use their defaults.
 * ------------------------------------------------------------------------- */

#[derive(serde::Deserialize, Default)]
#[serde(deny_unknown_fields)]
pub struct CartConfig {
    /// Target frames per second (default: 60). Compiled into .cart.config.
    #[serde(default = "default_fps")]
    pub fps: u8,

    /// Named record type declarations (flat POD structs).
    #[serde(default)]
    pub records: BTreeMap<String, RecordDecl>,

    /// Named state buffer pool declarations.
    #[serde(default)]
    pub state_buffers: BTreeMap<String, BufferDecl>,
}

fn default_fps() -> u8 {
    60
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RecordDecl {
    pub fields: Vec<FieldDecl>,
}

#[derive(serde::Deserialize, Clone)]
#[serde(deny_unknown_fields)]
pub struct FieldDecl {
    pub name: String,
    #[serde(rename = "type")]
    pub type_name: String,
}

#[derive(serde::Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BufferDecl {
    pub record: String,
    pub count: u32,
}

/* -------------------------------------------------------------------------
 * type_tag encoding (must match ecall.h BUF_TYPE_* and state_buffer.c)
 * 0=i8  1=u8  2=i16  3=u16  4=i32  5=u32  6=f32  7=bool
 * ------------------------------------------------------------------------- */

pub const TYPE_I8: u8 = 0;
pub const TYPE_U8: u8 = 1;
pub const TYPE_I16: u8 = 2;
pub const TYPE_U16: u8 = 3;
pub const TYPE_I32: u8 = 4;
pub const TYPE_U32: u8 = 5;
pub const TYPE_F32: u8 = 6;
pub const TYPE_BOOL: u8 = 7;

pub fn parse_type_tag(name: &str) -> Option<u8> {
    match name {
        "i8" => Some(TYPE_I8),
        "u8" => Some(TYPE_U8),
        "i16" => Some(TYPE_I16),
        "u16" => Some(TYPE_U16),
        "i32" => Some(TYPE_I32),
        "u32" => Some(TYPE_U32),
        "f32" => Some(TYPE_F32),
        "bool" => Some(TYPE_BOOL),
        _ => None,
    }
}

pub fn type_tag_c_type(tag: u8) -> &'static str {
    match tag {
        TYPE_I8 => "int8_t",
        TYPE_U8 => "uint8_t",
        TYPE_I16 => "int16_t",
        TYPE_U16 => "uint16_t",
        TYPE_I32 => "int32_t",
        TYPE_U32 => "uint32_t",
        TYPE_F32 => "float",
        TYPE_BOOL => "bool",
        _ => "uint8_t",
    }
}

pub fn type_tag_rust_type(tag: u8) -> &'static str {
    match tag {
        TYPE_I8 => "i8",
        TYPE_U8 => "u8",
        TYPE_I16 => "i16",
        TYPE_U16 => "u16",
        TYPE_I32 => "i32",
        TYPE_U32 => "u32",
        TYPE_F32 => "f32",
        TYPE_BOOL => "bool",
        _ => "u8",
    }
}

pub fn type_tag_buf_get_suffix(tag: u8) -> &'static str {
    match tag {
        TYPE_I8 => "i8",
        TYPE_U8 => "u8",
        TYPE_I16 => "i16",
        TYPE_U16 => "u16",
        TYPE_I32 => "i32",
        TYPE_U32 => "u32",
        TYPE_F32 => "f32",
        TYPE_BOOL => "bool",
        _ => "u8",
    }
}

/* -------------------------------------------------------------------------
 * Flattened field list: resolves inline record embedding into primitive fields.
 * A FlatField carries the dotted path name (e.g. "pos_x"), the primitive
 * type_tag, and the index within the flattened record.
 * ------------------------------------------------------------------------- */

#[derive(Clone)]
pub struct FlatField {
    /// Flattened field name — dots replaced by underscores (e.g. "pos_x")
    pub flat_name: String,
    pub type_tag: u8,
    /// 1-based index within this record's flattened field list
    pub index: u32,
}

/// Flatten all fields of a record, resolving inline embedding.
/// Returns Err if a type name is unknown or a cycle is detected.
pub fn flatten_record<'a>(
    record_name: &str,
    records: &'a BTreeMap<String, RecordDecl>,
    visiting: &mut Vec<String>,
) -> Result<Vec<FlatField>, String> {
    if visiting.contains(&record_name.to_string()) {
        return Err(format!(
            "blyt.config.yaml: cyclic record reference: {record_name} -> {}",
            visiting.join(" -> ")
        ));
    }
    let rec = records
        .get(record_name)
        .ok_or_else(|| format!("blyt.config.yaml: record {record_name:?} not declared"))?;

    visiting.push(record_name.to_string());

    let mut flat = Vec::new();
    for field in &rec.fields {
        if let Some(tag) = parse_type_tag(&field.type_name) {
            flat.push(FlatField {
                flat_name: field.name.clone(),
                type_tag: tag,
                index: 0, // filled in below
            });
        } else {
            // Inline embedding: field.type_name names another record
            let sub = flatten_record(&field.type_name, records, visiting)?;
            for sub_field in sub {
                flat.push(FlatField {
                    flat_name: format!("{}_{}", field.name, sub_field.flat_name),
                    type_tag: sub_field.type_tag,
                    index: 0,
                });
            }
        }
    }

    visiting.pop();

    // Assign 1-based indices
    for (i, f) in flat.iter_mut().enumerate() {
        f.index = (i + 1) as u32;
    }

    Ok(flat)
}

/* -------------------------------------------------------------------------
 * Schema hash: FNV-64 over the canonical text representation of all
 * records + state_buffers declarations, in declaration order.
 * The same hash is stored in the .cart.layouts section and in save files.
 * ------------------------------------------------------------------------- */

pub fn compute_schema_hash(cfg: &CartConfig) -> u64 {
    let mut text = String::new();
    for (name, rec) in &cfg.records {
        let _ = write!(text, "record:{name}");
        for f in &rec.fields {
            let _ = write!(text, ",{}:{}", f.name, f.type_name);
        }
        text.push(';');
    }
    for (name, buf) in &cfg.state_buffers {
        let _ = write!(text, "buffer:{name}:{}:{}", buf.record, buf.count);
        text.push(';');
    }
    fnv64(text.as_bytes())
}

fn fnv64(data: &[u8]) -> u64 {
    const FNV_OFFSET: u64 = 14695981039346656037;
    const FNV_PRIME: u64 = 1099511628211;
    let mut hash = FNV_OFFSET;
    for &b in data {
        hash ^= b as u64;
        hash = hash.wrapping_mul(FNV_PRIME);
    }
    hash
}

/* -------------------------------------------------------------------------
 * Parse blyt.config.yaml from a cart project directory.
 * Returns default CartConfig if the file does not exist.
 * ------------------------------------------------------------------------- */

pub fn read_cart_config(project_dir: &Path) -> Result<CartConfig, String> {
    let path = project_dir.join("blyt.config.yaml");
    if !path.exists() {
        return Ok(CartConfig::default());
    }
    let text = std::fs::read_to_string(&path)
        .map_err(|e| format!("blyt.config.yaml: {e}"))?;
    let cfg: CartConfig = serde_yaml::from_str(&text)
        .map_err(|e| format!("blyt.config.yaml: {e}"))?;

    // Validate all state_buffer record references
    for (buf_name, buf) in &cfg.state_buffers {
        if !cfg.records.contains_key(&buf.record) {
            return Err(format!(
                "blyt.config.yaml: state_buffer {buf_name:?} references unknown record {:?}",
                buf.record
            ));
        }
    }

    // Validate all field type names (detect cycles + unknown types)
    for (rec_name, _) in &cfg.records {
        let mut visiting = Vec::new();
        flatten_record(rec_name, &cfg.records, &mut visiting)
            .map_err(|e| e)?;
    }

    Ok(cfg)
}
