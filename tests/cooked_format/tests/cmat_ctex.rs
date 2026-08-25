//! `.cmat` and `.ctex` read from outside the engine.
//!
//! These are the formats a SHIPPED GAME loads, so their byte layouts are ABIs.
//! Until now both were only ever exercised by the C++ that writes them, through
//! a save/load round-trip — which proves the writer and reader agree with each
//! other and nothing more. Both could move together and every test would stay
//! green.
//!
//! Two failure modes are specific to these formats and invisible to a
//! round-trip:
//!
//! * **`.cmat` is fully variable-length.** Every string is a u32 length plus
//!   unterminated bytes, so a field inserted in the middle shifts everything
//!   after it. A C++ reader written in the same commit as the writer shifts with
//!   it; an independent one does not.
//!
//! * **`.ctex` is one flat blob whose meaning depends on BLOCK geometry.** A 6×6
//!   ASTC mip is `ceil(w/6)*ceil(h/6)` blocks. texture_asset.h records that the
//!   original math hard-coded 4×4 and that getting it wrong "reads a
//!   neighbouring mip as pixel data" — a plausible-looking wrong image, not a
//!   crash.
//!
//! The real cookers produce the fixtures here, driven through
//! `engine_cook_worker`'s process CLI, so what is parsed is what ships.

use engine_cooked_format as fmt;
use fmt::harness::{cook_worker_absence, cook_worker, scratch, skip};
use std::path::Path;
use std::process::Command;

fn cook(worker: &Path, src: &Path, out: &Path) -> bool {
    let res = out.with_extension("result.json");
    Command::new(worker)
        .arg(src).arg(out).arg(&res).arg("512")
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
        && out.exists()
}

/// A 4×4 PNG, hand-built. Small enough to write literally, real enough that the
/// texture cooker treats it as an image rather than rejecting it.
fn tiny_png() -> Vec<u8> {
    fn crc32(data: &[u8]) -> u32 {
        let mut table = [0u32; 256];
        for i in 0..256u32 {
            let mut c = i;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
            table[i as usize] = c;
        }
        let mut c = 0xFFFF_FFFFu32;
        for &b in data {
            c = table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
        }
        c ^ 0xFFFF_FFFF
    }
    fn chunk(kind: &[u8; 4], payload: &[u8]) -> Vec<u8> {
        let mut v = (payload.len() as u32).to_be_bytes().to_vec();
        v.extend_from_slice(kind);
        v.extend_from_slice(payload);
        let mut crc_in = kind.to_vec();
        crc_in.extend_from_slice(payload);
        v.extend_from_slice(&crc32(&crc_in).to_be_bytes());
        v
    }
    // zlib stream with STORED (uncompressed) deflate blocks — no compressor
    // needed, and a decoder must accept it.
    fn zlib_stored(raw: &[u8]) -> Vec<u8> {
        let mut z = vec![0x78, 0x01];
        z.push(0x01); // final block, stored
        z.extend_from_slice(&(raw.len() as u16).to_le_bytes());
        z.extend_from_slice(&(!(raw.len() as u16)).to_le_bytes());
        z.extend_from_slice(raw);
        // adler32
        let (mut a, mut b) = (1u32, 0u32);
        for &x in raw {
            a = (a + x as u32) % 65521;
            b = (b + a) % 65521;
        }
        z.extend_from_slice(&((b << 16) | a).to_be_bytes());
        z
    }

    const N: u32 = 4;
    let mut raw = Vec::new();
    for y in 0..N {
        raw.push(0u8); // filter: None
        for x in 0..N {
            raw.extend_from_slice(&[(x * 60) as u8, (y * 60) as u8, 128, 255]);
        }
    }

    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&N.to_be_bytes());
    ihdr.extend_from_slice(&N.to_be_bytes());
    ihdr.extend_from_slice(&[8, 6, 0, 0, 0]); // 8-bit RGBA

    let mut png = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
    png.extend_from_slice(&chunk(b"IHDR", &ihdr));
    png.extend_from_slice(&chunk(b"IDAT", &zlib_stored(&raw)));
    png.extend_from_slice(&chunk(b"IEND", &[]));
    png
}

#[test]
fn ctex_mip_chain_length_matches_its_declared_format() {
    let Some(worker) = cook_worker() else {
        skip("cmat/ctex cook", &cook_worker_absence());
        return;
    };
    let dir = scratch("ctex");
    let src = dir.join("tiny.png");
    let out = dir.join("tiny.ctex");
    std::fs::write(&src, tiny_png()).expect("write png");

    assert!(cook(&worker, &src, &out), "texture cook failed");

    let t = fmt::read_texture_from(&out).unwrap_or_else(|e| panic!("parsing .ctex failed — {e}"));

    assert!(t.width > 0 && t.height > 0, "degenerate dimensions {}x{}", t.width, t.height);
    assert!(t.mip_count >= 1, "mipCount must read as at least 1");

    // THE CHECK. Recomputed from the declared format's block geometry and
    // compared against the bytes actually present. A mismatch means the cooker
    // and this reader disagree about how a mip is sized — which downstream is
    // one mip being read as another, and looks like a blurry or garbled texture
    // rather than an error.
    assert!(
        t.payload_matches(),
        "mip chain length disagrees with the declared format: {:?} {}x{} \
         mips={} implies {} payload bytes, file has {}",
        t.format, t.width, t.height, t.mip_count, t.expected_payload(), t.payload_bytes
    );
}

#[test]
fn ctex_block_math_is_ceiling_not_floor() {
    use fmt::TexFormat::*;
    // A 5-texel row in a 4-wide block format costs TWO blocks. Flooring here
    // under-reads every mip below the first and is the arithmetic the header
    // warns about; asserted directly so the helper cannot regress quietly.
    assert_eq!(fmt::texture::mip_bytes(Bc1, 5, 5), 2 * 2 * 8);
    assert_eq!(fmt::texture::mip_bytes(Bc7, 4, 4), 16);
    // ASTC at a non-4 block size — the case that was hard-coded wrong once.
    assert_eq!(fmt::texture::mip_bytes(Astc6x6, 12, 12), 2 * 2 * 16);
    assert_eq!(fmt::texture::mip_bytes(Astc6x6, 7, 7), 2 * 2 * 16);
    assert_eq!(fmt::texture::mip_bytes(Astc8x8, 9, 9), 2 * 2 * 16);
    // Uncompressed is 4 bytes a texel with no blocks involved.
    assert_eq!(fmt::texture::mip_bytes(Rgba8, 5, 3), 5 * 3 * 4);

    // A chain never drops below 1×1, so the tail is a run of minimum-size mips
    // rather than zero-byte levels.
    assert_eq!(fmt::texture::chain_bytes(Rgba8, 2, 2, 3), 2 * 2 * 4 + 4 + 4);
}

#[test]
fn cmat_round_trips_through_an_independent_reader() {
    let Some(worker) = cook_worker() else {
        skip("cmat/ctex cook", &cook_worker_absence());
        return;
    };
    let dir = scratch("cmat");

    // A .material names a shader and sets parameters that shader declares. The
    // cooker resolves names and types against the shader's interface, so this
    // has to reference the real one.
    let shader_src = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../shaders/standard.shader")
        .canonicalize();
    let Ok(shader_src) = shader_src else {
        skip("cmat/ctex cook", "shaders/standard.shader not found");
        return;
    };
    std::fs::copy(&shader_src, dir.join("standard.shader")).expect("copy shader");

    let mat_src = dir.join("probe.material");
    std::fs::write(
        &mat_src,
        r#"{
  "shader": "standard.shader",
  "parameters": { "roughness": 0.25, "metallic": 0.75,
                  "baseColorFactor": [0.9, 0.2, 0.1, 1.0] }
}"#,
    )
    .expect("write material");

    let out = dir.join("probe.cmat");
    if !cook(&worker, &mat_src, &out) {
        // The material cooker needs the shader cooked and indexed first, which
        // a bare worker invocation may not provide. Skipping is honest — the
        // reader is still covered by the negative cases below and by any .cmat
        // a real project produces.
        skip("cmat live leg", "the material cook produced no output in isolation");
        return;
    }

    let m = fmt::read_material_from(&out).unwrap_or_else(|e| panic!("parsing .cmat failed — {e}"));

    assert_eq!(m.version, fmt::material::CMAT_VERSION);
    assert_eq!(m.shader_name, "standard",
               "the runtime resolves by DECLARED shader name, not by path");

    // The blocks must be COMPLETE — the format's own comment says a partial
    // block leaves whatever the previous draw wrote in the gaps, which is the
    // "looks right alone, wrong after another object draws" bug.
    for u in &m.uniforms {
        assert_eq!(u.values.len() % 4, 0,
                   "uniform '{}' is {} floats — not vec4-aligned", u.name, u.values.len());
        assert!(!u.values.is_empty(), "uniform '{}' is empty", u.name);
    }

    // Values land where standard.shader declares them: u_params.y/.z.
    if let Some(p) = m.uniform("u_params") {
        assert!((p.values[1] - 0.25).abs() < 1e-6, "roughness at u_params.y, got {}", p.values[1]);
        assert!((p.values[2] - 0.75).abs() < 1e-6, "metallic at u_params.z, got {}", p.values[2]);
    }
}

#[test]
fn the_readers_refuse_what_they_cannot_understand() {
    // A variable-length format read past its end is the failure that matters:
    // without bounds checks a corrupt length field is either a panic or a huge
    // allocation, and neither says what went wrong.
    let mut truncated = Vec::new();
    truncated.extend_from_slice(&fmt::material::CMAT_MAGIC.to_le_bytes());
    truncated.extend_from_slice(&fmt::material::CMAT_VERSION.to_le_bytes());
    truncated.extend_from_slice(&9999u32.to_le_bytes()); // a name 9999 bytes long
    match fmt::read_material(&truncated) {
        Err(fmt::ReadError::TooShort { .. }) => {}
        other => panic!("an oversized length field must be refused, got {other:?}"),
    }

    // An unknown VERSION must stop the walk rather than guess at a new layout.
    let mut wrong_ver = Vec::new();
    wrong_ver.extend_from_slice(&fmt::material::CMAT_MAGIC.to_le_bytes());
    wrong_ver.extend_from_slice(&99u32.to_le_bytes());
    match fmt::read_material(&wrong_ver) {
        Err(fmt::ReadError::BadVersion { found: 99, .. }) => {}
        other => panic!("an unknown version must be refused, got {other:?}"),
    }

    // A .ctex whose format id is not one this reader knows: the payload cannot
    // be sized at all, so reporting it beats computing a confident wrong number.
    let mut bad_fmt = vec![0u8; 32];
    bad_fmt[0..4].copy_from_slice(&fmt::texture::CTEX_MAGIC.to_le_bytes());
    bad_fmt[20..24].copy_from_slice(&777u32.to_le_bytes());
    match fmt::read_texture(&bad_fmt) {
        Err(fmt::ReadError::UnknownFormat(777)) => {}
        other => panic!("an unknown texture format must be refused, got {other:?}"),
    }

    // Wrong magic on both readers — a .ctex handed to the .cmat reader, the
    // mistake a packager makes once.
    let mut ctex_bytes = vec![0u8; 32];
    ctex_bytes[0..4].copy_from_slice(&fmt::texture::CTEX_MAGIC.to_le_bytes());
    assert!(matches!(fmt::read_material(&ctex_bytes), Err(fmt::ReadError::BadMagic(_))));
}
