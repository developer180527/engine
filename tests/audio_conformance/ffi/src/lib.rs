//! The audio conformance suite, callable from C (WO-093).
//!
//! A shipped player links its audio provider statically, so there is no
//! module for `cargo test` to load. The players CI packages to test link this
//! library instead (engine_build --conformance), and `engine_player
//! --conformance audio` hands it the provider table the player itself linked.
//! It runs `engine_audio_conformance::SUITE`, the same list `cargo test` runs
//! against a loaded module: one list, two ways in.
//!
//! A game's player never links this (DR-0032); shipped_player_check.py checks
//! that the package built without it does not contain it.
use engine_audio_conformance as conf;
use std::os::raw::c_int;

/// Runs every part of the suite against `provider`, printing each report.
/// Returns the number of failed checks: 0 is a pass. -1: no provider table,
/// -2: the suite panicked (a defect in it or in the provider; printed).
///
/// # Safety
/// `provider` must be null or a valid `EngineAudioProviderV1` that outlives the call.
#[no_mangle]
pub unsafe extern "C" fn engine_audio_conformance_run(provider: *const conf::ProviderV1) -> c_int {
    if provider.is_null() {
        eprintln!("[conformance] audio: no provider table");
        return -1;
    }
    let p: &conf::ProviderV1 = &*provider;
    let run = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let mut failures = 0usize;
        for (label, part) in conf::SUITE {
            let report = part(p);
            report.print(&format!("audio {label} (linked into this player)"));
            failures += report.failures();
        }
        failures
    }));
    match run {
        Ok(n) => n.min(c_int::MAX as usize) as c_int,
        Err(_) => {
            eprintln!("[conformance] audio: the suite panicked");
            -2
        }
    }
}
