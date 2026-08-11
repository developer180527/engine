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
    let report = unsafe { conf::run(&conf::reference::REFERENCE) };
    assert_eq!(report.failures(), 0, "reference must pass its own contract first");
    let seen = conf::reference::LAST_WELL_FORMED.load(std::sync::atomic::Ordering::Relaxed);
    assert_eq!(seen, 8,
        "the reference saw {seen} well-formed rows, not 8 — the wide-stride array \
         was walked with the wrong step, which is what silently misreads every \
         row after the first when the engine extends EmitterUpdate");
}

#[test]
fn reference_provider_conforms() {
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
fn native_provider_conforms() {
    let Ok(path) = std::env::var("ENGINE_AUDIO_PROVIDER") else {
        println!("ENGINE_AUDIO_PROVIDER unset — skipping the native provider.");
        println!("Point it at a built provider module to hold it to this contract.");
        return;
    };
    let p = match conf::load(&path) {
        Ok(p) => p,
        Err(e) => panic!("could not load provider '{path}': {e}"),
    };
    let report = unsafe { conf::run(p) };
    report.print(&format!("native provider ({path})"));
    assert_eq!(report.failures(), 0, "native provider violated the contract");
}
