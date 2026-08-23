//! `.cshader` and the DDC record manifest, read from outside the engine.
//!
//! Both are formats the engine writes and only the engine has ever read. A C++
//! save/load round-trip inflates the same numbers the writer emitted, so it
//! agrees with itself regardless of whether those numbers mean anything.
//!
//! What that misses, specifically:
//!
//! * **`.cshader` variants index bytecode by OFFSET AND SIZE into a blob**, and
//!   nothing in the format requires those ranges to be inside it. A variant
//!   pointing past the end is bytecode read beyond the allocation and handed to
//!   a GPU driver. Same shape for `ShaderParam::offset`, a float component in a
//!   shared vec4: offset 7 writes into whatever register follows.
//!
//! * **A DDC manifest's member names become filesystem paths** beside the
//!   primary output. A separator or a `..` in one is a write outside the
//!   intended directory, and the C++ side rejects rather than sanitizes.
//!
//! `fuzz_ddc_manifest_test` already asks "does the parser survive garbage" — and
//! found a real bug doing it. This asks the other question: given a manifest the
//! engine WROTE, does it say what it should. A writer that stopped emitting the
//! primary member is well-formed by every rule a fuzzer knows.

use engine_cooked_format as fmt;
use fmt::harness::{cook_worker, repo, scratch, skip};
use std::process::Command;

// ── .cshader ────────────────────────────────────────────────────────────────

#[test]
fn cshader_variants_point_inside_their_blob() {
    let Some(worker) = cook_worker() else {
        skip("cshader cook", "ENGINE_BUILD_DIR unset or engine_cook_worker missing");
        return;
    };
    let dir = scratch("cshader");

    // The real standard shader, with its .sc sources beside it — the cooker
    // resolves vertex/fragment/varying relative to the manifest.
    let shaders = repo().join("shaders");
    if !shaders.join("standard.shader").exists() {
        skip("cshader cook", "shaders/standard.shader missing");
        return;
    }
    for entry in std::fs::read_dir(&shaders).expect("read shaders/") {
        let p = entry.expect("dir entry").path();
        if p.is_file() {
            let _ = std::fs::copy(&p, dir.join(p.file_name().unwrap()));
        }
    }

    let src = dir.join("standard.shader");
    let out = dir.join("standard.cshader");
    let res = dir.join("standard.result.json");
    let ok = Command::new(&worker)
        .arg(&src).arg(&out).arg(&res).arg("512")
        .status().map(|s| s.success()).unwrap_or(false);
    if !ok || !out.exists() {
        // Shader cooking shells out to bgfx's shaderc. Running `cargo test` by
        // hand on a host without it, the honest outcome is a skip rather than a
        // failure about something this test is not examining. Under the CMake
        // entry it is a failure: shaderc is built as part of bgfx, so if the
        // cook did not produce output something is actually broken.
        skip("cshader cook", "the cook produced no output (shaderc unavailable?)");
        return;
    }

    let sh = fmt::read_shader_from(&out).unwrap_or_else(|e| panic!("parsing .cshader failed — {e}"));

    assert_eq!(sh.name, "standard", "the runtime resolves by DECLARED name");
    assert!(!sh.variants.is_empty(), "a cooked shader with no variants draws nothing");
    assert!(sh.blob_size > 0, "no bytecode in the blob");

    // THE CHECK. Every variant's vs/fs range must land inside the blob. Nothing
    // in the format enforces it and the C++ reader never questions it.
    sh.variant_ranges_in_bounds().unwrap_or_else(|e| panic!("{e}"));

    // A parameter packs into a vec4 at a float offset; offset + components must
    // fit in 4 or it spills into the next register.
    sh.params_fit_their_uniforms().unwrap_or_else(|e| panic!("{e}"));

    // A mask bit with no declared feature is a variant nothing can ask for.
    sh.feature_masks_are_declared().unwrap_or_else(|e| panic!("{e}"));

    // standard.shader declares roughness and metallic packed into u_params at
    // components 1 and 2 — the same offsets Material::standard synthesizes
    // against. If these ever disagree, mesh-embedded materials upload into the
    // wrong register and every surface shades wrong, silently.
    // PRESENCE IS ASSERTED, not assumed. These four were written as
    // `if let Some(p) = sh.param("roughness") { assert_eq!(...) }`, which checks
    // the offset when the param is there and passes silently when it is not — so
    // renaming or dropping `roughness` would have disarmed the guard instead of
    // tripping it. That is precisely the class of self-agreeing check this whole
    // crate exists to replace.
    let expect_param = |name: &str, uniform: &str, offset: u32| {
        let p = sh.param(name).unwrap_or_else(|| panic!(
            "standard.shader no longer declares '{name}' — this assertion is the \
             guard on Material::kStd* and a missing param DISARMS it rather than \
             failing it. If the rename is intended, move the constant in \
             src/render/material.h and update this name."));
        assert_eq!((p.uniform.as_str(), p.offset), (uniform, offset),
                   "'{name}' moved within {uniform} — the matching constant in \
                    src/render/material.h must move with it, or mesh-embedded \
                    materials upload into the wrong register and every surface \
                    shades wrong, silently");
    };
    expect_param("roughness", "u_params", 1);   // Material::kStdRoughness
    expect_param("metallic",  "u_params", 2);   // Material::kStdMetallic

    let expect_sampler = |name: &str, stage: u32| {
        let s = sh.sampler(name).unwrap_or_else(|| panic!(
            "standard.shader no longer declares sampler '{name}' — see above: a \
             missing sampler disarms this check rather than failing it"));
        assert_eq!(s.stage, stage, "sampler '{name}' moved to stage {}", s.stage);
    };
    expect_sampler("baseColor", 0);
    expect_sampler("normalMap", 1);

    // The closed-feature-list rule, from shader_asset.h: 32 features is 4
    // billion variants, and anything approaching the cap means the rule that
    // keeps the matrix cookable in full has already been abandoned.
    assert!(sh.features.len() <= fmt::shader::MAX_SHADER_FEATURES,
            "{} features declared, cap is {}", sh.features.len(), fmt::shader::MAX_SHADER_FEATURES);
}

#[test]
fn cshader_reader_refuses_what_it_cannot_understand() {
    // Out-of-bounds variant ranges, built by hand — the case a cooked file
    // should never contain and nothing currently checks.
    let mut a = fmt::ShaderAsset {
        version: fmt::shader::CSHD_VERSION,
        name: "probe".into(),
        features: vec![],
        params: vec![],
        samplers: vec![],
        variants: vec![fmt::shader::ShaderVariant {
            feature_mask: 0, profile: 0,
            vs_offset: 0, vs_size: 64,
            fs_offset: 64, fs_size: 64,
        }],
        blob_size: 100,   // 128 bytes of ranges in a 100-byte blob
    };
    assert!(a.variant_ranges_in_bounds().is_err(),
            "a variant running past the blob must be reported");

    a.blob_size = 128;
    assert!(a.variant_ranges_in_bounds().is_ok());

    // An empty range binds nothing and draws nothing.
    a.variants[0].fs_size = 0;
    assert!(a.variant_ranges_in_bounds().is_err(), "an empty fs range must be reported");

    // A param spilling out of its vec4.
    let b = fmt::ShaderAsset {
        params: vec![fmt::shader::ShaderParam {
            name: "spills".into(),
            ty: fmt::shader::ParamType::Vec3,
            uniform: "u_params".into(),
            offset: 2,          // 2 + 3 = 5 > 4
            defaults: [0.0; 4],
        }],
        ..a.clone()
    };
    assert!(b.params_fit_their_uniforms().is_err(),
            "a vec3 at offset 2 of a vec4 must be reported");

    // A mask bit with no declared feature.
    let c = fmt::ShaderAsset {
        features: vec!["SKINNED".into()],           // one feature -> bit 0 only
        variants: vec![fmt::shader::ShaderVariant {
            feature_mask: 0b10, profile: 0,
            vs_offset: 0, vs_size: 8, fs_offset: 8, fs_size: 8,
        }],
        blob_size: 16,
        ..a.clone()
    };
    assert!(c.feature_masks_are_declared().is_err(),
            "a mask bit outside the declared features must be reported");

    // Wrong magic and unknown version.
    let mut bad = Vec::new();
    bad.extend_from_slice(&0xDEAD_BEEFu32.to_le_bytes());
    assert!(matches!(fmt::read_shader(&bad), Err(fmt::ReadError::BadMagic(_))));

    let mut ver = Vec::new();
    ver.extend_from_slice(&fmt::shader::CSHD_MAGIC.to_le_bytes());
    ver.extend_from_slice(&99u32.to_le_bytes());
    assert!(matches!(fmt::read_shader(&ver), Err(fmt::ReadError::BadVersion { found: 99, .. })));

    // A length field larger than the file, which without a bounds check is
    // either a panic or an enormous allocation.
    let mut huge = Vec::new();
    huge.extend_from_slice(&fmt::shader::CSHD_MAGIC.to_le_bytes());
    huge.extend_from_slice(&fmt::shader::CSHD_VERSION.to_le_bytes());
    huge.extend_from_slice(&0xFFFF_FF00u32.to_le_bytes());
    assert!(matches!(fmt::read_shader(&huge), Err(fmt::ReadError::TooShort { .. })));
}

// ── DDC manifest ────────────────────────────────────────────────────────────

#[test]
fn manifest_accepts_what_the_engine_writes() {
    // Exactly the shape ddcStoreRecord emits: magic line, primary "@" first,
    // then siblings by filename.
    let text = "ddc-manifest-v1\n\
                aaaa1111\t@\n\
                bbbb2222\tmesh_albedo.ctex\n\
                cccc3333\tmesh_normal.ctex\n";
    let m = fmt::read_manifest(text).expect("a well-formed manifest must parse");
    assert_eq!(m.members.len(), 3);
    assert_eq!(m.primary().map(|p| p.blob_key.as_str()), Some("aaaa1111"));
    assert_eq!(m.siblings().count(), 2);
}

#[test]
fn manifest_rejects_a_record_that_yields_no_primary() {
    // The bug fuzz_ddc_manifest_test found on its first run: a manifest with no
    // "@" member returned success having written nothing, so the caller
    // committed Ready with a cookedPath that did not exist. A record is only a
    // hit if it actually produces the primary output.
    let text = "ddc-manifest-v1\nbbbb2222\tsibling.ctex\n";
    match fmt::read_manifest(text) {
        Err(fmt::ddc_manifest::ManifestError::NoPrimary) => {}
        other => panic!("a manifest with no primary must be a MISS, got {other:?}"),
    }

    // Two primaries is ambiguous rather than generous.
    let two = "ddc-manifest-v1\naaaa\t@\nbbbb\t@\n";
    assert!(matches!(fmt::read_manifest(two),
                     Err(fmt::ddc_manifest::ManifestError::MultiplePrimaries(2))));
}

#[test]
fn manifest_member_names_cannot_escape_their_directory() {
    // A member name becomes a path beside the primary output. Every one of these
    // is a write outside the intended directory, and the C++ side refuses rather
    // than sanitizing — "a manifest we don't fully understand is a miss".
    for evil in [
        "../escape.ctex",
        "sub/dir.ctex",
        "back\\slash.ctex",
        "C:stream.ctex",
        "..",
        ".",
        "",
    ] {
        let text = format!("ddc-manifest-v1\naaaa\t@\nbbbb\t{evil}\n");
        assert!(
            fmt::read_manifest(&text).is_err(),
            "member name {evil:?} must be refused — it does not stay in its directory"
        );
    }

    // A control character in a name: a filesystem path, not a payload, and a
    // classic way to disguise what a name really is.
    let ctrl = "ddc-manifest-v1\naaaa\t@\nbbbb\tbad\u{7}name.ctex\n";
    assert!(fmt::read_manifest(ctrl).is_err(), "control characters must be refused");

    // Over the 255-byte limit.
    let long = format!("ddc-manifest-v1\naaaa\t@\nbbbb\t{}\n", "x".repeat(256));
    assert!(fmt::read_manifest(&long).is_err(), "an over-long name must be refused");

    // And the ordinary case still passes, so the rule is not simply "reject".
    assert!(fmt::read_manifest("ddc-manifest-v1\naaaa\t@\nbbbb\tfine_name-1.ctex\n").is_ok());
}

/// Invalid UTF-8 is REFUSED, not repaired — in all four places this crate turns
/// bytes into text.
///
/// `String::from_utf8_lossy` substitutes U+FFFD per bad byte and returns a string
/// that parses cleanly, so a corrupt field arrived looking like a slightly odd
/// field. In the manifest it was worse than cosmetic: the repair ran BEFORE
/// `is_plain_filename`, so the check that decides whether a member name can
/// escape its directory was inspecting text the engine had never written, while
/// the C++ validates the raw bytes.
#[test]
fn text_fields_are_refused_rather_than_repaired() {
    // ── the manifest ────────────────────────────────────────────────────────
    // 0xFF is not valid UTF-8 anywhere. Lossily repaired it becomes U+FFFD —
    // three bytes, none a separator, none a control character — so the repair
    // can only ever make a name look SAFER than the bytes really are.
    let mut m = b"ddc-manifest-v1\naaaa\t@\nbbbb\tbad".to_vec();
    m.push(0xFF);
    m.extend_from_slice(b"name.ctex\n");
    match fmt::ddc_manifest::read_manifest_bytes(&m) {
        Err(fmt::ddc_manifest::ManifestError::NotUtf8 { .. }) => {}
        other => panic!("a non-UTF-8 manifest must be refused, got {other:?}"),
    }
    // And the same bytes WITHOUT the bad byte still parse, so the rule is not
    // simply "reject".
    assert!(fmt::ddc_manifest::read_manifest_bytes(
        b"ddc-manifest-v1\naaaa\t@\nbbbb\tbadname.ctex\n").is_ok());

    // ── .cshader ────────────────────────────────────────────────────────────
    // A shader whose NAME field carries an invalid byte.
    let mut sh = Vec::new();
    sh.extend_from_slice(&fmt::shader::CSHD_MAGIC.to_le_bytes());
    sh.extend_from_slice(&fmt::shader::CSHD_VERSION.to_le_bytes());
    let name = b"stan\xffdard";
    sh.extend_from_slice(&(name.len() as u32).to_le_bytes());
    sh.extend_from_slice(name);
    match fmt::read_shader(&sh) {
        Err(fmt::ReadError::BadUtf8 { what, .. }) =>
            assert!(what.contains("cshader"), "the error should name the format: {what}"),
        other => panic!("a non-UTF-8 .cshader name must be refused, got {other:?}"),
    }

    // ── the mesh's NUL-padded fixed-width paths ─────────────────────────────
    // A different code path from the length-prefixed strings above: 512 bytes,
    // read up to the first NUL. An empty mesh with one embedded material whose
    // baseColor path carries an invalid byte.
    let mut mesh = vec![0u8; 80 + fmt::COOKED_MATERIAL_BYTES];
    mesh[0..4].copy_from_slice(&fmt::MESH_MAGIC.to_le_bytes());
    mesh[4..8].copy_from_slice(&1u32.to_le_bytes());          // version
    mesh[72..76].copy_from_slice(&1u32.to_le_bytes());        // materialCount
    let path = b"art/rock\xffalbedo.png";                     // baseColor path at +28
    mesh[80 + 28..80 + 28 + path.len()].copy_from_slice(path);
    match fmt::read_materials(&mesh) {
        Err(fmt::ReadError::BadUtf8 { what, .. }) =>
            assert!(what.contains("baseColor"),
                    "the error should name the field: {what}"),
        other => panic!("a non-UTF-8 embedded texture path must be refused, got {other:?}"),
    }

    // The same mesh with a clean path parses, so this is a rule and not a
    // blanket refusal.
    let mut ok_mesh = mesh.clone();
    ok_mesh[80 + 28..80 + 28 + path.len()].fill(0);
    ok_mesh[80 + 28..80 + 28 + 8].copy_from_slice(b"rock.png");
    let mats = fmt::read_materials(&ok_mesh).expect("a clean path must parse");
    assert_eq!(mats[0].base_color_path, "rock.png");
}

#[test]
fn manifest_rejects_malformed_structure() {
    // Wrong magic — including the near-miss of a version bump nobody taught
    // this reader about.
    assert!(fmt::read_manifest("ddc-manifest-v2\naaaa\t@\n").is_err());
    assert!(fmt::read_manifest("").is_err());

    // A line with no tab, or with an empty half.
    for bad in ["ddc-manifest-v1\nnotabhere\n",
                "ddc-manifest-v1\n\t@\n",
                "ddc-manifest-v1\naaaa\t\n"] {
        assert!(fmt::read_manifest(bad).is_err(), "malformed line must be refused: {bad:?}");
    }
}
