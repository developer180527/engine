//! BUG-0034 — a cooked sibling path the result format could not carry.
//!
//! # The defect
//!
//! `engine_cook_worker` writes its result as a framed file: a header, a body of
//! `KEY value` lines, and an `END <line-count> <digest>` trailer. Two of those
//! records carry text that came from outside the worker — `ERROR`, which is a
//! message, and `OUTPUT`, which is a filesystem PATH the parent then OPENS and
//! registers as a cooked sibling.
//!
//! `ERROR` went through `oneLine`. `OUTPUT` did not.
//!
//! A newline is legal in a POSIX filename, and a sibling texture's path is
//! derived from the asset's own stem — cooking `foo.gltf` to `foo.cooked`
//! produces `foo_t0.ctex` beside it. So an asset whose name contained a newline
//! produced a body with one more line than the writer had counted. The frame
//! then failed its OWN line-count check, and the parent reported
//!
//!     its result file is unusable: END claims 2 line(s), found 3
//!
//! blaming the worker's write for what was really a filename. The cook itself
//! had succeeded.
//!
//! # Why the fix is a refusal and not a sanitise
//!
//! Sanitising is the tempting fix and the wrong one, and the difference is what
//! the caller does next. The parent OPENS the `OUTPUT` path. A mangled path is a
//! file that does not exist, so sanitising would trade a loud, if misdirected,
//! failure for a DDC record pointing at nothing — a silently missing sibling,
//! which is the exact class of bug the trailer was added to prevent. The same
//! line `engine/addon_protocol.h` draws between `record` and `recordExact`.
//!
//! # What this test pins, and what it would miss
//!
//! Both halves, because either alone is satisfiable by a broken worker:
//!
//!   * the newline-named asset produces a result that PARSES and says `fail`
//!     with the reason — not an unframeable file;
//!   * the identically-shaped clean asset still cooks to `ok` AND still emits
//!     the `OUTPUT` sibling record. Without that control, a worker that had
//!     stopped emitting `OUTPUT` records entirely — which is the silently
//!     missing sibling, arriving a different way — would pass the first check
//!     perfectly.
//!
//! The fixture needs an EXTERNAL image URI. A glTF with its texture embedded as
//! a base64 data URI cooks fine and emits no sibling at all, so it never reaches
//! the record that carries the path; that costs one extra file and is the
//! difference between this test exercising the bug and cooking a triangle.

use engine_cooked_format::harness::{cook_worker_absence, cook_worker, scratch, skip};
use std::path::Path;
use std::process::Command;

// ── The frame, reimplemented ────────────────────────────────────────────────
// A third independent implementation of the same framing, after the C++ writer
// and `engine_module_abi`'s reader. That is deliberate and is the crate's whole
// premise: a reader written beside its writer agrees with the writer by
// construction, and would follow it through a change to the digest seed or the
// trailer spelling without ever going red.
//
// Kept in this file rather than the crate's `src/`: one test needs it, and the
// crate's public surface is the COOKED ASSET formats. A result file is the cook
// pipeline's IPC, not an asset.
const COOK_MAGIC: &str = "ENGINE_COOK_RESULT";

fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for &c in bytes {
        h ^= c as u64;
        h = h.wrapping_mul(1099511628211);
    }
    h
}

/// Validate the frame and return the body lines. Every malformed shape is an
/// `Err`, never a panic — the whole point of the trailer is that this file may
/// have been written by a process that died partway through producing it, so
/// "malformed" is an expected input here rather than a bug.
fn unframe(file: &[u8]) -> Result<Vec<String>, String> {
    let h = file.iter().position(|&c| c == b'\n').ok_or("no header line")?;
    let header = std::str::from_utf8(&file[..h]).map_err(|_| "header not UTF-8")?;
    let ver = header
        .strip_prefix(COOK_MAGIC)
        .ok_or_else(|| format!("bad magic: {header:?}"))?
        .trim();
    if ver != "1" {
        return Err(format!("unexpected cook-result version {ver:?}"));
    }
    if file.last() != Some(&b'\n') {
        return Err("truncated: does not end at a line boundary".into());
    }
    let last_beg = file[..file.len() - 1]
        .iter()
        .rposition(|&c| c == b'\n')
        .ok_or("truncated: no END trailer")?;
    let trailer = std::str::from_utf8(&file[last_beg + 1..file.len() - 1])
        .map_err(|_| "trailer not UTF-8")?;
    let rest = trailer.strip_prefix("END ").ok_or_else(|| {
        format!("truncated: last line is not END (tool died mid-write): {trailer:?}")
    })?;
    let (lines_txt, hash_txt) = rest.split_once(' ').ok_or("malformed END trailer")?;
    let claimed_lines: usize = lines_txt.parse().map_err(|_| "END line count is not a number")?;
    let claimed_hash = u64::from_str_radix(hash_txt.trim(), 16)
        .map_err(|_| "END digest is not hex")?;

    let payload = &file[h + 1..=last_beg];
    let found = payload.iter().filter(|&&c| c == b'\n').count();
    if found != claimed_lines {
        // THE failure the bug produced. Spelled out, because a bare count
        // mismatch is exactly what sent the last person looking at the writer.
        return Err(format!(
            "incomplete: END claims {claimed_lines} line(s), found {found} — a record \
             carried an unescaped newline, or the writer died mid-body"
        ));
    }
    if fnv1a64(payload) != claimed_hash {
        return Err("corrupt: body digest does not match END".into());
    }
    Ok(String::from_utf8_lossy(payload).lines().map(str::to_string).collect())
}

// ── The fixture ─────────────────────────────────────────────────────────────

/// A 4×4 RGBA PNG, hand-built so the fixture needs no image library.
fn tiny_png() -> Vec<u8> {
    fn crc32(data: &[u8]) -> u32 {
        let mut c = 0xFFFF_FFFFu32;
        for &b in data {
            c ^= b as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
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
    // A stored (uncompressed) deflate block, so no compressor is needed and any
    // decoder must accept it.
    fn zlib_stored(raw: &[u8]) -> Vec<u8> {
        let mut z = vec![0x78, 0x01, 0x01];
        z.extend_from_slice(&(raw.len() as u16).to_le_bytes());
        z.extend_from_slice(&(!(raw.len() as u16)).to_le_bytes());
        z.extend_from_slice(raw);
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
        raw.push(0u8);
        for x in 0..N {
            raw.extend_from_slice(&[(x * 60) as u8, (y * 60) as u8, 128, 255]);
        }
    }
    let mut ihdr = Vec::new();
    ihdr.extend_from_slice(&N.to_be_bytes());
    ihdr.extend_from_slice(&N.to_be_bytes());
    ihdr.extend_from_slice(&[8, 6, 0, 0, 0]);
    let mut png = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
    png.extend_from_slice(&chunk(b"IHDR", &ihdr));
    png.extend_from_slice(&chunk(b"IDAT", &zlib_stored(&raw)));
    png.extend_from_slice(&chunk(b"IEND", &[]));
    png
}

/// One triangle with a base-colour texture referenced BY FILENAME. The external
/// URI is what makes the cook emit a sibling `.ctex`, which is the record that
/// carries a path.
fn textured_gltf() -> String {
    let mut buf: Vec<u8> = Vec::new();
    for v in [[0.0f32, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]] {
        for c in v {
            buf.extend_from_slice(&c.to_le_bytes());
        }
    }
    for i in [0u16, 1, 2] {
        buf.extend_from_slice(&i.to_le_bytes());
    }
    buf.extend_from_slice(&[0, 0]);

    const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut b64 = String::new();
    for ch in buf.chunks(3) {
        let b = [ch[0], *ch.get(1).unwrap_or(&0), *ch.get(2).unwrap_or(&0)];
        let n = ((b[0] as u32) << 16) | ((b[1] as u32) << 8) | b[2] as u32;
        b64.push(T[(n >> 18 & 63) as usize] as char);
        b64.push(T[(n >> 12 & 63) as usize] as char);
        b64.push(if ch.len() > 1 { T[(n >> 6 & 63) as usize] as char } else { '=' });
        b64.push(if ch.len() > 2 { T[(n & 63) as usize] as char } else { '=' });
    }

    format!(
        r#"{{
  "asset": {{ "version": "2.0" }},
  "scene": 0,
  "scenes": [ {{ "nodes": [0] }} ],
  "nodes": [ {{ "mesh": 0 }} ],
  "meshes": [ {{ "primitives": [ {{
      "attributes": {{ "POSITION": 0 }}, "indices": 1, "material": 0 }} ] }} ],
  "materials": [ {{ "name": "m", "pbrMetallicRoughness": {{
      "baseColorTexture": {{ "index": 0 }} }} }} ],
  "textures": [ {{ "source": 0 }} ],
  "images":   [ {{ "uri": "t.png" }} ],
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

/// Cook `src` to `out`, returning the parsed result-file body lines.
fn cook(worker: &Path, src: &Path, out: &Path, res: &Path) -> Result<Vec<String>, String> {
    let status = Command::new(worker)
        .arg(src).arg(out).arg(res).arg("512")
        .status()
        .map_err(|e| format!("could not run engine_cook_worker: {e}"))?;
    // The worker reports its verdict in the FILE; a non-zero status here would
    // mean it did not get that far, which is a different finding.
    if !status.success() {
        return Err(format!("worker exited {status} without reaching a verdict"));
    }
    let bytes = std::fs::read(res).map_err(|e| format!("no result file: {e}"))?;
    unframe(&bytes)
}

#[test]
fn a_sibling_path_the_format_cannot_carry_is_refused_not_mangled() {
    let Some(worker) = cook_worker() else {
        skip("cook result frame", &cook_worker_absence());
        return;
    };
    let dir = scratch("cook_result_frame");
    let outdir = dir.join("out");
    std::fs::create_dir_all(&outdir).expect("out dir");
    std::fs::write(dir.join("t.png"), tiny_png()).expect("write png");

    // ── The control, first ──────────────────────────────────────────────────
    // An identical asset under an ordinary name. It has to cook AND emit the
    // OUTPUT sibling, because a worker that had simply stopped reporting
    // siblings would sail through the newline case below while shipping the
    // silently-missing-sibling bug by another route.
    let clean_src = dir.join("clean.gltf");
    std::fs::write(&clean_src, textured_gltf()).expect("write gltf");
    let clean = cook(&worker, &clean_src, &outdir.join("clean.cooked"),
                     &dir.join("clean.result"))
        .unwrap_or_else(|e| panic!("the control asset did not produce a readable result — {e}"));

    assert_eq!(clean.first().map(String::as_str), Some("RESULT ok"),
               "the control asset should cook cleanly, got {clean:?}");
    let sibling = clean.iter().find(|l| l.starts_with("OUTPUT "))
        .unwrap_or_else(|| panic!(
            "the control cook emitted no OUTPUT record, so this test never reaches the \
             record that carries a path. Either the mesh cooker stopped emitting sibling \
             textures — itself the silently-missing-sibling bug — or the fixture stopped \
             producing one. Body: {clean:?}"));
    assert!(sibling.ends_with(".ctex"),
            "expected a cooked sibling texture, got {sibling:?}");

    // ── The defect ──────────────────────────────────────────────────────────
    // A newline in the asset's stem, which the sibling path inherits.
    let bad_src = dir.join("bad\nname.gltf");
    if std::fs::write(&bad_src, textured_gltf()).is_err() {
        // Windows rejects a newline in a filename outright, so the defect cannot
        // exist there. Skipping is the honest report; the control above still ran.
        skip("cook result frame (newline leg)",
             "this filesystem will not create a filename containing a newline");
        return;
    }

    let body = cook(&worker, &bad_src, &outdir.join("bad\nname.cooked"),
                    &dir.join("bad.result"))
        .unwrap_or_else(|e| panic!(
            "the result file for a newline-named asset did not parse — {e}\n\n\
             This is BUG-0034 itself: the OUTPUT record carried the path verbatim, so \
             the body gained a line the writer had not counted and the frame failed its \
             own check. The parent then blames the worker's write for what is a filename."));

    assert_eq!(body.first().map(String::as_str), Some("RESULT fail"),
               "an unrepresentable sibling path must FAIL the cook, not pass it, \
                got {body:?}");
    let err = body.iter().find(|l| l.starts_with("ERROR "))
        .unwrap_or_else(|| panic!("a failed cook must say why. Body: {body:?}"));
    assert!(err.contains("cannot carry"),
            "the error should name the format's limit so a human can act on it, got {err:?}");

    // No OUTPUT record survives. A refusal that still reported the path would be
    // the sanitise-instead-of-refuse fix: the parent would open a file that is
    // not there and register a DDC sibling pointing at nothing.
    assert!(!body.iter().any(|l| l.starts_with("OUTPUT ")),
            "a refused cook must report no outputs at all, got {body:?}");
}

#[test]
fn the_frame_reader_rejects_what_it_cannot_trust() {
    // The reader above is what turns the bug into a readable failure, so its own
    // refusals are worth pinning — a reader that accepted a bad line count would
    // report the defect as a pass.
    let good = b"ENGINE_COOK_RESULT 1\nRESULT ok\nEND 1 ".to_vec();
    let mut good = good;
    good.extend_from_slice(format!("{:x}\n", fnv1a64(b"RESULT ok\n")).as_bytes());
    assert_eq!(unframe(&good).unwrap(), vec!["RESULT ok".to_string()]);

    // The exact shape the bug produced: a body line more than END counted.
    let mut miscounted = b"ENGINE_COOK_RESULT 1\nRESULT ok\nOUTPUT /tmp/a\nb.ctex\nEND 2 ".to_vec();
    miscounted.extend_from_slice(
        format!("{:x}\n", fnv1a64(b"RESULT ok\nOUTPUT /tmp/a\nb.ctex\n")).as_bytes());
    match unframe(&miscounted) {
        Err(e) if e.contains("incomplete") => {}
        other => panic!("an under-counted body must be refused, got {other:?}"),
    }

    let truncated = b"ENGINE_COOK_RESULT 1\nRESULT ok\n";
    match unframe(truncated) {
        Err(e) if e.contains("not END") => {}
        other => panic!("a body with no trailer must be refused, got {other:?}"),
    }

    let mut corrupt = b"ENGINE_COOK_RESULT 1\nRESULT ok\nEND 1 ".to_vec();
    corrupt.extend_from_slice(b"deadbeefdeadbeef\n");
    match unframe(&corrupt) {
        Err(e) if e.contains("digest") => {}
        other => panic!("a wrong digest must be refused, got {other:?}"),
    }

    match unframe(b"ENGINE_ADDON_RESULT 1\nVERDICT ok\nEND 1 0\n") {
        Err(e) if e.contains("bad magic") => {}
        other => panic!("an add-on result handed to the cook reader must be refused, got {other:?}"),
    }
}
