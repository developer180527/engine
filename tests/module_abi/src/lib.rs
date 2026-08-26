//! Driving the engine's module load gauntlet from outside C++.
//!
//! # What is under test, and why it is not the same thing api_abi_compat_test covers
//!
//! `ModuleLibrary::load` decides whether a shared library is allowed to touch
//! live world data. It refuses for five reasons: struct size, API version, ABI
//! fingerprint, component layout hash, and kit-to-kit contract skew. Each one
//! exists to convert a specific silent memory-corruption bug into a message.
//!
//! The engine already tests the function-table LAYOUTS thoroughly — thirteen
//! frozen groups, every offset pinned. That is the static half, and it is
//! self-defending: a layout regression trips a `static_assert` before anything
//! runs. The gate is the dynamic half and it defends nothing on its own. Every
//! failure mode in it is quiet:
//!
//! * a comparison inverted — a bad module loads, and the log says nothing
//! * a `return refuse()` softened to `return false` — the module's table and
//!   instance leak on every rejection, on a path only the unhappy case reaches
//! * a check moved after the point of no return — the refusal message is right
//!   and the corruption has already happened
//!
//! None of those breaks a build or reddens a test. They surface days later as a
//! hot-reload that scrambles components.
//!
//! # Why this is Rust, and where that stops being the right call
//!
//! The rule this repo follows is that a Rust test needs a boundary that ALREADY
//! exists — otherwise a C shim gets written purely so the test can be in Rust,
//! which adds untested C++ to remove tested C++. Two real boundaries are in play
//! here and neither was invented for the test:
//!
//! * `engine_module_probe`'s command line and its **Add-on result format**. A CLI
//!   is a contract the same way `engine_cook_worker`'s is, and the probe answers
//!   a question ("why won't my kit load?") that a human asks independently of any
//!   test suite.
//! * the fixture modules themselves, which are a C ABI by construction.
//!
//! What is deliberately NOT done here is calling the loader directly. That is
//! C++ internals with C++ types, and reaching it from Rust would need exactly
//! the shim the rule forbids.
//!
//! # The format is reimplemented here, on purpose
//!
//! `unframe` below is an independent Rust implementation of the frame that
//! `engine/addon_protocol.h` writes — including its FNV-1a digest. It does not
//! bind to the C++ and it must not.
//!
//! That is the only real test of whether a format is SPECIFIED or merely
//! implemented. A test that calls the writer's own reader agrees with the writer
//! by construction: rename a key, invert a field, change the digest seed, and
//! both halves move together and stay green. Two implementations that must agree
//! on bytes cannot do that. It is also the honest simulation of the actual
//! client — an Add-on may be written by anyone, in anything, and will have
//! reimplemented this from the comments exactly as this file did.
//!
//! # Out of process, on purpose
//!
//! Each case is a fresh `engine_module_probe`. A refused module leaves state
//! behind that an in-process suite would have to unwind — the loader parks
//! rejected images in a process-lifetime graveyard rather than unmapping them,
//! and contract pins live in a process-global registry. Testing every defect in
//! one process would mean each case running against whatever the previous ones
//! left, and a leak in the refusal path — one of the things being tested — would
//! be indistinguishable from the graveyard doing its job.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicU32, Ordering};

/// Set by the CMake test entry. When it is on, a skip is a FAILURE.
///
/// The same guard the cooked-format lane uses, for the same reason: under CMake
/// the probe and all ten fixtures are known to exist, so "the inputs weren't
/// there" is a finding. `cargo test --quiet` captures stdout, so without this a
/// lane that had quietly stopped exercising anything would print `ok` exactly
/// like one that ran.
pub fn skips_are_failures() -> bool {
    std::env::var("ENGINE_REQUIRE_MODULE_ABI_TESTS")
        .map(|v| v != "0" && !v.is_empty())
        .unwrap_or(false)
}

pub fn skip(what: &str, reason: &str) {
    if skips_are_failures() {
        panic!("{what} SKIPPED under ENGINE_REQUIRE_MODULE_ABI_TESTS=1 — {reason}");
    }
    eprintln!("skipping {what}: {reason}");
}

/// The probe binary. `ENGINE_MODULE_PROBE` is an absolute path from CMake;
/// a bare `cargo test` from a shell has no engine built and gets `None`.
pub fn probe() -> Option<PathBuf> {
    let p = PathBuf::from(std::env::var("ENGINE_MODULE_PROBE").ok()?);
    p.is_file().then_some(p)
}

pub fn probe_absence() -> String {
    match std::env::var("ENGINE_MODULE_PROBE") {
        Ok(p) => format!("ENGINE_MODULE_PROBE={p} is not a file — build engine_module_probe"),
        Err(_) => "ENGINE_MODULE_PROBE is unset (run through ctest, or set it by hand)".into(),
    }
}

/// Where CMake writes the fixture modules.
pub fn gate_dir() -> Option<PathBuf> {
    let p = PathBuf::from(std::env::var("ENGINE_ABI_GATE_DIR").ok()?);
    p.is_dir().then_some(p)
}

/// Resolve a fixture by its CMake name, across the three platform spellings of
/// a MODULE library. Tried in order rather than picked by `cfg!`, because a
/// cross build's host and target can disagree and a wrong guess would look like
/// a missing fixture.
pub fn fixture(name: &str) -> Option<PathBuf> {
    let dir = gate_dir()?;
    for candidate in [
        format!("libabi_gate_{name}.so"),
        format!("libabi_gate_{name}.dylib"),
        format!("abi_gate_{name}.dll"),
    ] {
        let p = dir.join(candidate);
        if p.is_file() {
            return Some(p);
        }
    }
    None
}

// ── The Add-on protocol, reimplemented ──────────────────────────────────────

/// Bumped only when the frame or the reserved records change.
pub const PROTOCOL_VERSION: i32 = 1;
pub const RESULT_MAGIC: &str = "ENGINE_ADDON_RESULT";
pub const MANIFEST_MAGIC: &str = "ENGINE_ADDON_MANIFEST";

/// FNV-1a, 64-bit. `wrapping_mul` because the C++ relies on unsigned overflow
/// and Rust would panic on it in a debug build — which is how this function
/// silently produced the right answer in release and no answer at all in test.
pub fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for &c in bytes {
        h ^= c as u64;
        h = h.wrapping_mul(1099511628211);
    }
    h
}

/// Validate a framed Add-on file and return its body.
///
/// Every malformed shape is an `Err` with a reason a human can act on, never a
/// panic: this parses the output of a process that may have died in the middle
/// of writing it, so "malformed" is an expected input rather than a bug here.
pub fn unframe(magic: &str, file: &[u8]) -> Result<String, String> {
    let h = file
        .iter()
        .position(|&c| c == b'\n')
        .ok_or_else(|| "no header line".to_string())?;
    let header = &file[..h];
    if !header.starts_with(magic.as_bytes()) {
        return Err(format!("not an add-on {magic} file (bad magic)"));
    }
    let ver_txt = std::str::from_utf8(&header[magic.len()..])
        .map_err(|_| "header is not UTF-8".to_string())?
        .trim();
    let ver: i32 = ver_txt
        .parse()
        .map_err(|_| format!("header has no version (found {ver_txt:?})"))?;
    if ver != PROTOCOL_VERSION {
        return Err(format!(
            "add-on protocol version {ver}, expected {PROTOCOL_VERSION} \
             (stale tool binary?)"
        ));
    }

    // A file not ending in a newline was cut mid-line: the common truncation
    // shape, and the one a sentinel line cannot detect because the sentinel is
    // exactly what got cut.
    if file.last() != Some(&b'\n') {
        return Err("truncated: does not end at a line boundary".into());
    }
    let last_end = file.len() - 1;
    let last_beg = file[..last_end]
        .iter()
        .rposition(|&c| c == b'\n')
        .ok_or_else(|| "truncated: no END trailer".to_string())?;
    let trailer = std::str::from_utf8(&file[last_beg + 1..last_end])
        .map_err(|_| "trailer is not UTF-8".to_string())?;
    if !trailer.starts_with("END ") {
        return Err(format!(
            "truncated: last line is {trailer:?}, not END (tool died mid-write)"
        ));
    }
    let mut fields = trailer.split_whitespace().skip(1);
    let claimed_lines: usize = fields
        .next()
        .and_then(|f| f.parse().ok())
        .ok_or_else(|| "malformed END trailer: no line count".to_string())?;
    let claimed_hash: u64 = fields
        .next()
        .and_then(|f| u64::from_str_radix(f, 16).ok())
        .ok_or_else(|| "malformed END trailer: no digest".to_string())?;

    let payload = &file[h + 1..last_beg + 1];
    let lines = payload.iter().filter(|&&c| c == b'\n').count();
    if lines != claimed_lines {
        return Err(format!(
            "incomplete: END claims {claimed_lines} line(s), found {lines}"
        ));
    }
    if fnv1a64(payload) != claimed_hash {
        return Err("corrupt: body digest does not match END".into());
    }

    // STRICT UTF-8 here, unlike stdout and stderr below. Those are human
    // channels where a mangled byte costs nothing but a smudged message. This is
    // the machine channel, and a body that is not UTF-8 means either corruption
    // or a path this format cannot carry — both of which deserve to be named
    // rather than replaced with U+FFFD and parsed anyway.
    String::from_utf8(payload.to_vec())
        .map_err(|e| format!("body is not UTF-8: {e}"))
}

/// Split a validated body into `(KEY, value)` pairs, in order.
pub fn records(body: &str) -> Vec<(String, String)> {
    body.lines()
        .filter_map(|l| {
            let (k, v) = l.split_once(' ').unwrap_or((l, ""));
            (!k.is_empty()).then(|| (k.to_string(), v.to_string()))
        })
        .collect()
}

/// What the probe reported about one module.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Verdict {
    Ok,
    Refused,
}

#[derive(Debug)]
pub struct ProbeRun {
    /// Read from the framed result FILE — never from stdout. See `run`.
    pub verdicts: Vec<Verdict>,
    pub status: Option<i32>,
    /// The host's own LOG_ERROR text. Reported in assertion messages so a
    /// failure says WHICH check fired, but never matched against — the wording
    /// is for humans and pinning it would make improving a message a test break.
    pub stderr: String,
    pub stdout: String,
    /// The raw bytes of the result file, for tests that inspect the framing
    /// itself rather than what it carried.
    pub result_bytes: Vec<u8>,
    /// The `VERDICT` reserved record: `ok` whenever the probe RAN, refusals
    /// included, because a refusal is a successful probe of a bad module.
    pub overall: String,
    /// The validated result body, so a test can examine which record KEYS a run
    /// actually emitted rather than only the values it knows how to read.
    pub body: String,
}

impl ProbeRun {
    /// The probe reached the end and said so.
    ///
    /// Replaces the old `probe done` sentinel on stdout, and is strictly
    /// stronger than it was. A sentinel proves a line was printed; a valid frame
    /// with an agreeing line count and digest proves the whole body arrived —
    /// and unlike a sentinel, a loaded module cannot forge it, because it is not
    /// on a channel the module can write to.
    pub fn ran(&self) -> bool {
        self.status == Some(0) && self.overall == "ok"
    }

    pub fn only(&self) -> Verdict {
        assert_eq!(
            self.verdicts.len(),
            1,
            "expected exactly one module verdict, got {:?}\nstdout:\n{}\nstderr:\n{}",
            self.verdicts, self.stdout, self.stderr
        );
        self.verdicts[0]
    }
}

/// A unique result-file path per call, so concurrent `cargo test` threads cannot
/// read each other's answers.
fn next_result_path() -> PathBuf {
    static N: AtomicU32 = AtomicU32::new(0);
    let n = N.fetch_add(1, Ordering::Relaxed);
    std::env::temp_dir().join(format!(
        "engine_addon_result_{}_{n}.txt",
        std::process::id()
    ))
}

/// Run the probe over some modules, in order, in one process.
///
/// `trace` names a file the fixtures append their lifecycle events to; passing
/// `None` leaves `ABI_GATE_TRACE` unset and the fixtures write nothing.
///
/// # Why a missing result file is a panic and not an empty run
///
/// The probe exits 0 exactly when it RAN, and a run that ran owes a result file.
/// Falling back to parsing stdout when the file is absent would make this
/// conversion decorative: a writer that broke would quietly revert to the
/// forgeable channel and every test here would keep passing. So a zero exit with
/// no readable result is a hard failure with the frame error in the message.
pub fn run(modules: &[&Path], reload_never: bool, trace: Option<&Path>) -> ProbeRun {
    let exe = probe().expect("probe() checked by the caller");
    let result_path = next_result_path();
    let _ = std::fs::remove_file(&result_path);

    let mut cmd = Command::new(exe);
    if reload_never {
        cmd.arg("--reload=never");
    }
    cmd.arg(format!("--addon-result={}", result_path.display()));
    for m in modules {
        cmd.arg(m);
    }
    match trace {
        Some(t) => {
            let _ = std::fs::remove_file(t);
            cmd.env("ABI_GATE_TRACE", t);
        }
        // Removed rather than left alone: cargo inherits the caller's
        // environment, and a stale ABI_GATE_TRACE from an earlier run would
        // have fixtures appending to a file this case never reads.
        None => {
            cmd.env_remove("ABI_GATE_TRACE");
        }
    }

    let out = cmd.output().expect("failed to run engine_module_probe");
    // Lossy for the HUMAN channels, and only there. These exist to be quoted
    // into assertion messages; a replacement character in a log line costs
    // nothing, and refusing to decode one would lose the diagnostic entirely.
    let stdout = String::from_utf8_lossy(&out.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&out.stderr).into_owned();
    let status = out.status.code();

    let result_bytes = std::fs::read(&result_path).unwrap_or_default();
    let _ = std::fs::remove_file(&result_path);

    let mut verdicts = Vec::new();
    let mut overall = String::new();
    let mut body = String::new();

    if status == Some(0) {
        body = unframe(RESULT_MAGIC, &result_bytes).unwrap_or_else(|e| {
            panic!(
                "engine_module_probe exited 0 but its result file is unusable: {e}\n\
                 The probe exits 0 only when it RAN, and a run that ran owes a \
                 readable result. This is not a fixture problem — it is the tool \
                 breaking its own Add-on contract.\n\
                 result path: {}\n{} byte(s)\nstdout:\n{stdout}\nstderr:\n{stderr}",
                result_path.display(),
                result_bytes.len()
            )
        });
        for (key, value) in records(&body) {
            match key.as_str() {
                "VERDICT" => overall = value,
                // MODULE <index> <ok|refused> <path>
                "MODULE" => {
                    let f: Vec<&str> = value.splitn(3, ' ').collect();
                    match f.get(1).copied() {
                        Some("ok") => verdicts.push(Verdict::Ok),
                        Some("refused") => verdicts.push(Verdict::Refused),
                        other => panic!(
                            "MODULE record has verdict {other:?}, which is neither \
                             'ok' nor 'refused': {value:?}"
                        ),
                    }
                }
                _ => {}
            }
        }
    }

    ProbeRun { verdicts, status, stderr, stdout, result_bytes, overall, body }
}

/// The probe's Add-on manifest, as `(KEY, value)` pairs.
///
/// Loads nothing and runs no module, which is why the manifest may travel on
/// stdout at all: the channel is only contended once untrusted code is in the
/// process.
pub fn manifest() -> Vec<(String, String)> {
    let exe = probe().expect("probe() checked by the caller");
    let out = Command::new(exe)
        .arg("--addon-manifest")
        .output()
        .expect("failed to run engine_module_probe --addon-manifest");
    assert_eq!(
        out.status.code(),
        Some(0),
        "--addon-manifest must exit 0; stderr:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    let body = unframe(MANIFEST_MAGIC, &out.stdout)
        .unwrap_or_else(|e| panic!("--addon-manifest produced an unusable frame: {e}"));
    records(&body)
}

/// The events one fixture recorded, in order, as `(defect_code, event)`.
pub fn read_trace(path: &Path) -> Vec<(i32, String)> {
    let Ok(text) = std::fs::read_to_string(path) else {
        return Vec::new();
    };
    text.lines()
        .filter_map(|l| {
            let (code, event) = l.split_once(' ')?;
            Some((code.trim().parse().ok()?, event.trim().to_string()))
        })
        .collect()
}

/// A scratch directory for this test's trace files, cleared on entry.
pub fn scratch(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("engine_module_abi_{name}"));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("scratch dir");
    d
}
