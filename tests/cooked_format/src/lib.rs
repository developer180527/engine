//! Reading the engine's cooked mesh format from outside C++.
//!
//! ## Why this crate exists
//!
//! A `.cooked` file is what a SHIPPED GAME reads. That makes its byte layout a
//! real ABI — as much as `engine_api_table.h` is — and it has never been checked
//! from anywhere except the C++ that writes it. A round-trip through
//! `saveMesh`/`loadMesh` proves the two agree with each other, which is a weaker
//! statement than it looks: both could move together and no test would notice.
//!
//! Parsing it here, from a hand-written layout with no engine code and no
//! generated bindings, is the other half — the same relationship
//! `tests/audio_abi_check.c` has with the Rust audio suite, pointed the other
//! way. It also settles a question the format's existence implies: can a tool
//! that is not this engine read a cooked asset? A packager, a validator, an
//! asset browser written in something else.
//!
//! ## Layout, from `modules/assetlib/include/assetlib/mesh_asset.h`
//!
//! ```text
//! MeshHeader        80 bytes   (static_assert'd in the C++ header)
//! vertexData        vertexCount * vertexStride
//! indexData         indexCount  * indexStride
//! submeshes         submeshCount  * 32     (MeshSubmesh)
//! materials         materialCount * 1052   (CookedMaterial)
//! ...               skinned payload, LODs — not parsed here
//! ```
//!
//! The sizes are transcribed rather than derived, so a change on the C++ side
//! that shifts the payload shows up as a parse failure here instead of as
//! plausible-looking garbage.

pub mod ddc_manifest;
pub mod harness;
pub mod material;
pub mod shader;
pub mod texture;

pub use ddc_manifest::{read_manifest, read_manifest_from, Manifest};
pub use material::{read_material, read_material_from, MaterialAsset};
pub use shader::{read_shader, read_shader_from, ShaderAsset};
pub use texture::{read_texture, read_texture_from, TexFormat, TextureAsset};

use std::path::Path;

pub const MESH_MAGIC: u32 = 0x4D45_5348; // 'MESH'
pub const MESH_HEADER_BYTES: usize = 80;
pub const SUBMESH_BYTES: usize = 32;
pub const COOKED_MATERIAL_BYTES: usize = 1052;

#[derive(Debug, Clone, Copy)]
pub struct MeshHeader {
    pub magic: u32,
    pub version: u32,
    pub vertex_flags: u32,
    pub vertex_stride: u32,
    pub vertex_count: u32,
    pub index_count: u32,
    pub index_stride: u32,
    pub submesh_count: u32,
    pub material_count: u32,
    pub bone_count: u32,
}

/// One embedded material, as the mesh cookers write it.
#[derive(Debug, Clone)]
pub struct CookedMaterial {
    pub base_color_factor: [f32; 4],
    pub roughness: f32,
    pub metallic: f32,
    pub flags: u32,
    pub base_color_path: String,
    pub normal_map_path: String,
}

#[derive(Debug)]
pub enum ReadError {
    Io(String),
    TooShort { need: usize, have: usize },
    BadMagic(u32),
    BadVersion { found: u32, expected: u32 },
    /// The walk finished before the file did. For a variable-length format that
    /// means the writer emits a section this reader does not know about, which
    /// is indistinguishable from a forgotten version bump — and silently
    /// ignoring it is how a reader drifts out of date without anyone noticing.
    TrailingBytes { parsed: usize, total: usize },
    UnknownFormat(u32),
    Manifest(String),
}

impl std::fmt::Display for ReadError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ReadError::Io(e) => write!(f, "io: {e}"),
            ReadError::TooShort { need, have } => write!(
                f,
                "file is shorter than the layout requires: need {need} bytes, have {have} \
                 — the C++ layout and this transcription have diverged"
            ),
            ReadError::BadMagic(m) => write!(f, "bad magic {m:#x}"),
            ReadError::BadVersion { found, expected } => write!(
                f,
                "format version {found}, this reader knows {expected} — bump the \
                 reader deliberately rather than letting it guess at a new layout"
            ),
            ReadError::TrailingBytes { parsed, total } => write!(
                f,
                "parsed {parsed} of {total} bytes — the writer emits a section this \
                 reader does not know about"
            ),
            ReadError::UnknownFormat(v) => write!(f, "unknown enum id {v}"),
            ReadError::Manifest(m) => write!(f, "ddc manifest: {m}"),
        }
    }
}

fn u32_at(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}
fn f32_at(b: &[u8], off: usize) -> f32 {
    f32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}
/// A fixed-width NUL-padded char array. Stops at the first NUL; anything after
/// it is padding the writer did not clear and is not content.
fn cstr_at(b: &[u8], off: usize, cap: usize) -> String {
    let s = &b[off..off + cap];
    let end = s.iter().position(|&c| c == 0).unwrap_or(cap);
    String::from_utf8_lossy(&s[..end]).into_owned()
}

pub fn read_header(bytes: &[u8]) -> Result<MeshHeader, ReadError> {
    if bytes.len() < MESH_HEADER_BYTES {
        return Err(ReadError::TooShort { need: MESH_HEADER_BYTES, have: bytes.len() });
    }
    let magic = u32_at(bytes, 0);
    if magic != MESH_MAGIC {
        return Err(ReadError::BadMagic(magic));
    }
    // Offsets follow the declaration order in mesh_asset.h: magic, version,
    // uuid[16], then the counts.
    Ok(MeshHeader {
        magic,
        version: u32_at(bytes, 4),
        vertex_flags: u32_at(bytes, 24),
        vertex_stride: u32_at(bytes, 28),
        vertex_count: u32_at(bytes, 32),
        index_count: u32_at(bytes, 36),
        index_stride: u32_at(bytes, 40),
        submesh_count: u32_at(bytes, 44),
        // boundsMin[3] and boundsMax[3] occupy 48..72.
        material_count: u32_at(bytes, 72),
        bone_count: u32_at(bytes, 76),
    })
}

/// Every embedded material in a cooked mesh.
pub fn read_materials(bytes: &[u8]) -> Result<Vec<CookedMaterial>, ReadError> {
    let h = read_header(bytes)?;
    // The material section sits after the variable-length payloads. Computing
    // the offset rather than searching for it is the point: if the C++ side
    // grows a section without this crate learning about it, the parse lands in
    // the wrong place and the assertions fail loudly instead of reading
    // plausible floats out of vertex data.
    let mut off = MESH_HEADER_BYTES;
    off += h.vertex_count as usize * h.vertex_stride as usize;
    off += h.index_count as usize * h.index_stride as usize;
    off += h.submesh_count as usize * SUBMESH_BYTES;

    let need = off + h.material_count as usize * COOKED_MATERIAL_BYTES;
    if bytes.len() < need {
        return Err(ReadError::TooShort { need, have: bytes.len() });
    }

    let mut out = Vec::with_capacity(h.material_count as usize);
    for i in 0..h.material_count as usize {
        let m = off + i * COOKED_MATERIAL_BYTES;
        out.push(CookedMaterial {
            base_color_factor: [
                f32_at(bytes, m),
                f32_at(bytes, m + 4),
                f32_at(bytes, m + 8),
                f32_at(bytes, m + 12),
            ],
            roughness: f32_at(bytes, m + 16),
            metallic: f32_at(bytes, m + 20),
            flags: u32_at(bytes, m + 24),
            base_color_path: cstr_at(bytes, m + 28, 512),
            normal_map_path: cstr_at(bytes, m + 540, 512),
        });
    }
    Ok(out)
}

pub fn read_materials_from(path: &Path) -> Result<Vec<CookedMaterial>, ReadError> {
    let bytes = std::fs::read(path).map_err(|e| ReadError::Io(e.to_string()))?;
    read_materials(&bytes)
}
