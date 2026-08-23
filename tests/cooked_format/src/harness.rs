//! Locating the cook worker, and the difference between "cannot run here" and
//! "did not run, and nobody noticed".
//!
//! ## The failure this exists to stop
//!
//! Most tests in this crate build structs by hand and need nothing. A few cook a
//! REAL asset with `engine_cook_worker` and read the bytes back — and those are
//! the only ones that can catch the writer drifting, which is the entire premise
//! of the crate. They each began with a `println!` and an early `return` when the
//! worker was missing.
//!
//! `cargo test --quiet` — which is how CMake invokes this — CAPTURES stdout. So a
//! test that skipped every single run printed its reason into a buffer nobody
//! reads and reported `ok`. A lane that has quietly stopped exercising anything
//! is indistinguishable, in the only output anyone looks at, from a lane that
//! passes. This repo has already shipped that exact shape: `ENGINE_AUDIO_FROZEN`
//! carried static asserts for months in a header no translation unit included,
//! so not one of them had ever compiled.
//!
//! ## The rule
//!
//! * A bare `cargo test` by hand SKIPS. There is no build directory to point at,
//!   the developer knows it, and failing would be noise.
//! * Under `ENGINE_REQUIRE_COOK_TESTS=1` — which the CMake test entry sets — a
//!   skip is a FAILURE. If the harness went to the trouble of naming a build
//!   directory, then the worker, the shaders and the cook are all supposed to be
//!   there, and any one of them missing is a finding rather than a shrug.
//!
//! That split is deliberate: the environment that can tell whether a skip is
//! legitimate is the one that set up the environment.

use std::path::{Path, PathBuf};

/// Set by the CMake test entry. Its presence means "this run is expected to be
/// able to cook", so a skip becomes a failure.
pub fn skips_are_failures() -> bool {
    std::env::var_os("ENGINE_REQUIRE_COOK_TESTS").is_some_and(|v| v != "0")
}

/// Report that a test cannot proceed.
///
/// Panics — i.e. fails the test — when the harness declared cooking mandatory.
/// Otherwise prints, and the caller returns. Always call it as
/// `harness::skip(...); return;` so the control flow is visible at the call
/// site rather than hidden in a return type.
pub fn skip(what: &str, reason: &str) {
    if skips_are_failures() {
        panic!(
            "{what}: {reason}\n\
             ENGINE_REQUIRE_COOK_TESTS is set, so this run was expected to cook. \
             A skip here is the finding: it means the lane that reads REAL cooked \
             bytes did not run, and every other test in this crate only checks \
             structs built by hand."
        );
    }
    println!("{what}: {reason} — skipping (set ENGINE_REQUIRE_COOK_TESTS=1 to make this fail).");
}

/// `engine_cook_worker` from the build directory CMake pointed us at.
pub fn cook_worker() -> Option<PathBuf> {
    let dir = std::env::var("ENGINE_BUILD_DIR").ok()?;
    let p = Path::new(&dir).join("engine_cook_worker");
    p.exists().then_some(p)
}

/// The repository root, from this crate's manifest directory.
pub fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .canonicalize()
        .expect("repo root")
}

/// A clean per-test scratch directory under the system temp dir.
pub fn scratch(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("engine_cooked_format_{name}"));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(&d).expect("scratch dir");
    d
}
