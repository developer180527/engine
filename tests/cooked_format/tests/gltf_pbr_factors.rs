//! BUG-0012 — what the mesh cooker records for a glTF that omits its PBR factors.
//!
//! ## The bug, corrected
//!
//! `fps_shooter`'s pistol renders fully metallic and fully rough. The first
//! diagnosis blamed a change to `gltf_importer.cpp` made during the Phase 5
//! material migration. That was WRONG, and reading the cooked file settled it:
//! the `.cooked` already contained `roughness=1.000 metallic=1.000` — written by
//! the mesh COOKER, on a path the direct importer never touches. The scene loads
//! the pistol through `cookedPath`.
//!
//! What actually happens, and none of it is a mistake in isolation:
//!
//!   * `pistol_without_mag.gltf` omits `roughnessFactor` and `metallicFactor`.
//!   * glTF says an absent factor is **1.0**, so cgltf reports 1.0/1.0. The file
//!     is not underspecified — it is specifying 1.0.
//!   * The cooker records that faithfully.
//!   * glTF defines those factors to MULTIPLY a metallicRoughness texture, and
//!     the forward pipeline samples only baseColor and normal. With nothing to
//!     multiply, 1.0/1.0 is not "the authored look", it is an unapplied
//!     coefficient standing in for a map nobody reads.
//!
//! So the defect is a RENDERER gap, not a cook error, and the honest fix is
//! sampling the MR/ARM texture. Until that exists this test pins the value the
//! cooker writes, so it is a visible, deliberate number instead of a surprise
//! found by looking at a pistol.
//!
//! ## Why this test is in Rust, and where that stops being a good idea
//!
//! It drives the real cooker through `engine_cook_worker`'s PROCESS CLI and
//! parses the `.cooked` bytes from a transcribed layout. Both of those are
//! genuine ABIs — a command line and a file format — so no C shim exists purely
//! to be tested, and the test is exercising the same surface a shipped game and
//! any third-party tool would use.
//!
//! That is the condition. Testing a C++ API with C++ types from Rust would mean
//! writing a C shim first, which ADDS untested C++ for the privilege of testing
//! in Rust. The boundary has to already exist.

use std::path::{Path, PathBuf};
use std::process::Command;

/// A minimal glTF with one material whose `pbrMetallicRoughness` is present but
/// EMPTY — no `roughnessFactor`, no `metallicFactor`. That is the exact shape
/// the pistol has, and the shape most exporters emit.
///
/// Geometry is one triangle with an inline base64 buffer, because the point is
/// the material record and a mesh still has to parse for the cook to reach it.
fn tiny_gltf_without_factors() -> String {
    // 3 positions (f32x3) = 36 bytes, then 3 u16 indices = 6 bytes, padded to 4.
    // Written out as base64 so the fixture is a single self-contained file.
    let mut buf: Vec<u8> = Vec::new();
    for v in [
        [0.0f32, 0.0, 0.0],
        [1.0f32, 0.0, 0.0],
        [0.0f32, 1.0, 0.0],
    ] {
        for c in v {
            buf.extend_from_slice(&c.to_le_bytes());
        }
    }
    for i in [0u16, 1, 2] {
        buf.extend_from_slice(&i.to_le_bytes());
    }
    buf.extend_from_slice(&[0, 0]); // pad to a 4-byte boundary

    let b64 = base64(&buf);
    format!(
        r#"{{
  "asset": {{ "version": "2.0" }},
  "scene": 0,
  "scenes": [ {{ "nodes": [0] }} ],
  "nodes": [ {{ "mesh": 0 }} ],
  "meshes": [ {{ "primitives": [ {{
      "attributes": {{ "POSITION": 0 }}, "indices": 1, "material": 0 }} ] }} ],
  "materials": [ {{ "name": "no_factors", "pbrMetallicRoughness": {{}} }} ],
  "accessors": [
    {{ "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3",
       "min": [0,0,0], "max": [1,1,0] }},
    {{ "bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR" }}
  ],
  "bufferViews": [
    {{ "buffer": 0, "byteOffset": 0,  "byteLength": 36 }},
    {{ "buffer": 0, "byteOffset": 36, "byteLength": 6 }}
  ],
  "buffers": [ {{ "byteLength": {}, "uri": "data:application/octet-stream;base64,{}" }} ]
}}"#,
        buf.len(),
        b64
    )
}

fn base64(data: &[u8]) -> String {
    const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for chunk in data.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let n = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
        out.push(T[(n >> 18 & 63) as usize] as char);
        out.push(T[(n >> 12 & 63) as usize] as char);
        out.push(if chunk.len() > 1 { T[(n >> 6 & 63) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { T[(n & 63) as usize] as char } else { '=' });
    }
    out
}

/// The cooker binary, from the build directory CMake told us about.
fn cook_worker() -> Option<PathBuf> {
    let dir = std::env::var("ENGINE_BUILD_DIR").ok()?;
    let p = Path::new(&dir).join("engine_cook_worker");
    p.exists().then_some(p)
}

#[test]
fn gltf_without_pbr_factors_cooks_to_one_point_zero() {
    let Some(worker) = cook_worker() else {
        // Registered by CMake with ENGINE_BUILD_DIR set; a bare `cargo test`
        // from a shell has no engine to drive and skips rather than failing on
        // something that is not the test's subject.
        println!("ENGINE_BUILD_DIR unset or engine_cook_worker missing — skipping.");
        return;
    };

    let dir = std::env::temp_dir().join("engine_cooked_format_gltf");
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("temp dir");

    let src = dir.join("no_factors.gltf");
    let out = dir.join("no_factors.cooked");
    let res = dir.join("result.json");
    std::fs::write(&src, tiny_gltf_without_factors()).expect("write fixture");

    let status = Command::new(&worker)
        .arg(&src)
        .arg(&out)
        .arg(&res)
        .arg("512")
        .status()
        .expect("run engine_cook_worker");
    assert!(status.success(), "cook failed: {status}");
    assert!(out.exists(), "cooker produced no output at {}", out.display());

    let mats = engine_cooked_format::read_materials_from(&out)
        .unwrap_or_else(|e| panic!("parsing the cooked mesh failed — {e}"));

    assert_eq!(
        mats.len(),
        1,
        "expected the glTF's single material to be embedded, got {}",
        mats.len()
    );
    let m = &mats[0];

    // THE PINNED VALUE. glTF's absent-factor default is 1.0, cgltf reports it,
    // and the cooker records it. Asserting it here makes the number deliberate:
    // if someone later clamps it to the engine defaults, or starts multiplying a
    // metallicRoughness texture, this fails and they have to say so.
    assert_eq!(
        (m.roughness, m.metallic),
        (1.0, 1.0),
        "the cooker no longer records glTF's absent-factor default of 1.0. That may \
         be an improvement — but it changes how every glTF without explicit factors \
         shades, so it must be a decision, not a side effect. See BUG-0012."
    );

    // Base colour has the same absent-default rule and is unaffected, because
    // white multiplies to nothing. Asserted so the two are not confused: only
    // roughness/metallic lack a texture to multiply against.
    assert_eq!(m.base_color_factor, [1.0, 1.0, 1.0, 1.0]);

    // No textures in the fixture, so both path fields must be EMPTY rather than
    // holding uninitialised bytes from the 512-byte arrays.
    assert!(
        m.base_color_path.is_empty() && m.normal_map_path.is_empty(),
        "untextured material carried path bytes: base='{}' normal='{}'",
        m.base_color_path,
        m.normal_map_path
    );
}

#[test]
fn the_transcribed_layout_rejects_garbage() {
    // The parser computes the material offset from the header's counts. If the
    // C++ layout grows a section this crate does not know about, that arithmetic
    // lands somewhere wrong — and the failure has to be loud, because floats
    // read out of vertex data look perfectly plausible.
    let mut fake = vec![0u8; 80];
    fake[0..4].copy_from_slice(&engine_cooked_format::MESH_MAGIC.to_le_bytes());
    // Claim one material, but supply no bytes for it.
    fake[72..76].copy_from_slice(&1u32.to_le_bytes());
    match engine_cooked_format::read_materials(&fake) {
        Err(engine_cooked_format::ReadError::TooShort { .. }) => {}
        other => panic!("a truncated material section must be refused, got {other:?}"),
    }

    let mut bad_magic = vec![0u8; 80];
    bad_magic[0..4].copy_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
    match engine_cooked_format::read_materials(&bad_magic) {
        Err(engine_cooked_format::ReadError::BadMagic(_)) => {}
        other => panic!("a non-mesh file must be refused, got {other:?}"),
    }
}
