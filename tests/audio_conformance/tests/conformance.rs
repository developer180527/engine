//! Runs the contract against every provider we can reach.
//!
//! The reference provider always runs. A NATIVE provider runs too when
//! ENGINE_AUDIO_PROVIDER points at a module — that is how the engine's own
//! miniaudio implementation gets held to the same standard as a third party's,
//! which is the only way the seam stays real rather than decorative.
use engine_audio_conformance as conf;

#[test]
fn reference_stride_is_walked_by_stride() {
    // The forward-compat property the `stride` parameter exists for, asserted on
    // VALUES rather than on "it did not crash". The suite sends 8 rows spaced by
    // a stride 16 bytes wider than the struct, with the gaps poisoned to 0xFF; a
    // provider that walks by size_of::<EmitterUpdate>() lands on that poison from
    // row 1 onward and rejects it. Only a provider using the stride sees 8.
    //
    // Read back through a test-fixture accessor, not the ABI — a native provider
    // has no such channel, which is why the in-crate reference exists.
    // Held across BOTH the run and the read: LAST_WELL_FORMED is process-global
    // and the other reference test drives the same counter on another thread.
    let _guard = conf::reference::REFERENCE_LOCK.lock().unwrap();
    let report = unsafe { conf::run(&conf::reference::REFERENCE) };
    assert_eq!(report.failures(), 0, "reference must pass its own contract first");
    let seen = conf::reference::LAST_WELL_FORMED.load(std::sync::atomic::Ordering::Relaxed);
    assert_eq!(seen, 8,
        "the reference saw {seen} well-formed rows, not 8 — the wide-stride array \
         was walked with the wrong step, which is what silently misreads every \
         row after the first when the engine extends EmitterUpdate");
}

#[test]
fn hash_matches_the_c_header() {
    // findSound and setParam address content by name hash, so the engine's C
    // implementation and a provider's own must agree EXACTLY or every lookup
    // silently misses — no crash, no error, just a sound that never plays.
    //
    // These expectations were measured by compiling engineAudioHashName from
    // engine_audio_provider.h and printing the result, in both C11 and C++20.
    // Pinning literals rather than calling the C function is deliberate: an FFI
    // call would compare the header against itself and could never catch drift
    // between two independent implementations, which is the actual risk.
    assert_eq!(
        conf::hash_name("Play_Gunshot"),
        7_375_508_369_329_266_918,
        "FNV-1a 64 diverged from the C header — every findSound would miss"
    );
    // The empty string must land on the unmodified FNV offset basis; getting
    // the seed wrong is the classic way two implementations disagree.
    assert_eq!(conf::hash_name(""), 0xCBF2_9CE4_8422_2325);
    // Distinct names must not collide in the trivial way a broken multiply
    // produces.
    assert_ne!(conf::hash_name("a"), conf::hash_name("b"));
}

#[test]
fn reference_provider_conforms() {
    let _guard = conf::reference::REFERENCE_LOCK.lock().unwrap();
    let report = unsafe { conf::run(&conf::reference::REFERENCE) };
    report.print("rust reference provider");
    assert_eq!(
        report.failures(),
        0,
        "the reference provider must satisfy its own contract — if it cannot, \
         the contract is unimplementable and no third party will manage it"
    );
}

#[test]
fn native_provider_passes_the_suite() {
    // Every part of conf::SUITE: the contract (a provider says NO correctly)
    // and playback (it can say yes: real WAV bytes, decoded, played, clocked,
    // streamed from memory and pulled through a reader; the reference decodes
    // nothing, so that part only means something against a native provider).
    // The same list a CI-built shipped player runs against the provider it
    // linked (ffi/, WO-093), so the two cannot hold a provider to different
    // rules.
    let Ok(path) = std::env::var("ENGINE_AUDIO_PROVIDER") else {
        println!("ENGINE_AUDIO_PROVIDER unset — skipping the native provider.");
        println!("Point it at a built provider module to hold it to this contract.");
        return;
    };
    let p = match conf::load(&path) {
        Ok(p) => p,
        Err(e) => panic!("could not load provider '{path}': {e}"),
    };
    for (label, part) in conf::SUITE {
        let report = unsafe { part(p) };
        report.print(&format!("native provider {label} ({path})"));
        assert_eq!(report.failures(), 0, "native provider failed the suite's {label} part");
    }
}
