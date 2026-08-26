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
//! * `engine_module_probe`'s command line and its stdout format. A CLI is a
//!   contract the same way `engine_cook_worker`'s is, and the probe answers a
//!   question ("why won't my kit load?") that a human asks independently of any
//!   test suite.
//! * the fixture modules themselves, which are a C ABI by construction.
//!
//! What is deliberately NOT done here is calling the loader directly. That is
//! C++ internals with C++ types, and reaching it from Rust would need exactly
//! the shim the rule forbids.
//!
//! # Out of process, on purpose
//!
//! Each case is a fresh `engine_module_probe`. A refused module leaves state
//! behind that an in-process suite would have to unwind — the loader parks
//! rejected images in a process-lifetime graveyard rather than unmapping them,
//! and contract pins live in a process-global registry. Testing nine defects in
//! one process would mean each case running against whatever the previous eight
//! left, and a leak in the refusal path — one of the things being tested — would
//! be indistinguishable from the graveyard doing its job.

use std::path::{Path, PathBuf};
use std::process::Command;

/// Set by the CMake test entry. When it is on, a skip is a FAILURE.
///
/// The same guard the cooked-format lane uses, for the same reason: under CMake
/// the probe and all nine fixtures are known to exist, so "the inputs weren't
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

/// What the probe reported about one module.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Verdict {
    Ok,
    Refused,
}

#[derive(Debug)]
pub struct ProbeRun {
    pub verdicts: Vec<Verdict>,
    pub status: Option<i32>,
    /// The host's own LOG_ERROR text. Reported in assertion messages so a
    /// failure says WHICH check fired, but never matched against — the wording
    /// is for humans and pinning it would make improving a message a test break.
    pub stderr: String,
    pub stdout: String,
    /// False when `probe done` is absent: the run died partway through.
    pub completed: bool,
}

impl ProbeRun {
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

/// Run the probe over some modules, in order, in one process.
///
/// `trace` names a file the fixtures append their lifecycle events to; passing
/// `None` leaves `ABI_GATE_TRACE` unset and the fixtures write nothing.
pub fn run(modules: &[&Path], reload_never: bool, trace: Option<&Path>) -> ProbeRun {
    let exe = probe().expect("probe() checked by the caller");
    let mut cmd = Command::new(exe);
    if reload_never {
        cmd.arg("--reload=never");
    }
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
    let stdout = String::from_utf8_lossy(&out.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&out.stderr).into_owned();

    let mut verdicts = Vec::new();
    let mut completed = false;
    for line in stdout.lines() {
        let f: Vec<&str> = line.split_whitespace().collect();
        match f.as_slice() {
            ["module", _, "ok", ..] => verdicts.push(Verdict::Ok),
            ["module", _, "refused", ..] => verdicts.push(Verdict::Refused),
            ["probe", "done"] => completed = true,
            _ => {}
        }
    }

    ProbeRun { verdicts, status: out.status.code(), stderr, stdout, completed }
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
