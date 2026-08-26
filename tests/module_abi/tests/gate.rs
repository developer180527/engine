//! The load gauntlet, one deliberate defect at a time.
//!
//! Every test here is the same shape: build a module that is correct in every
//! respect but one, hand it to the real host loader, and require a refusal. The
//! control case — a fixture with no defect at all — is what stops the suite
//! degenerating into "the loader rejects everything", which would pass just as
//! green with the gate stubbed out to `return false`.

use engine_module_abi as m;
use engine_module_abi::Verdict;
use std::path::PathBuf;

/// Resolve the probe and a set of fixtures, or skip with a reason.
///
/// Returns `None` after calling `skip`, so each test's early return is visible
/// at its own call site rather than hidden in a helper.
fn setup(what: &str, names: &[&str]) -> Option<Vec<PathBuf>> {
    if m::probe().is_none() {
        m::skip(what, &m::probe_absence());
        return None;
    }
    let mut out = Vec::new();
    for n in names {
        match m::fixture(n) {
            Some(p) => out.push(p),
            None => {
                m::skip(what, &format!("fixture 'abi_gate_{n}' was not built"));
                return None;
            }
        }
    }
    Some(out)
}

fn probe_one(paths: &[PathBuf]) -> m::ProbeRun {
    let refs: Vec<&std::path::Path> = paths.iter().map(|p| p.as_path()).collect();
    m::run(&refs, false, None)
}

// ── The control ─────────────────────────────────────────────────────────────

/// A module with NO defect must load.
///
/// This is the test that gives the other eight their meaning. Without it, a
/// gauntlet that refused unconditionally — a misplaced `return refuse()`, a
/// check comparing a field against itself — would satisfy every "must be
/// refused" assertion in this file and look completely healthy, while in reality
/// no kit could ever load again.
#[test]
fn a_correct_module_is_accepted() {
    let Some(f) = setup("abi gate control", &["good"]) else { return };
    let run = probe_one(&f);
    assert_eq!(
        run.only(),
        Verdict::Ok,
        "a module matching the host in every gate field was REFUSED. Every other \
         test in this file asserts a refusal, so this one failing alone means \
         the gauntlet rejects everything and the suite would still be green.\n\
         host said:\n{}",
        run.stderr
    );
    assert!(run.completed, "probe did not finish:\n{}", run.stdout);
}

// ── The five refusals ───────────────────────────────────────────────────────

/// `structSize` understated: the module was built against a different SDK.
///
/// Understating is the dangerous direction — the host would read table fields
/// past the end of the module's own allocation — so that is the direction the
/// fixture takes.
#[test]
fn a_wrong_struct_size_is_refused() {
    let Some(f) = setup("abi gate struct size", &["struct_size"]) else { return };
    let run = probe_one(&f);
    assert_eq!(run.only(), Verdict::Refused,
               "a module reporting the wrong structSize was accepted; the host \
                would read past the end of its table.\nhost said:\n{}", run.stderr);
}

/// `apiVersion` bumped: same compiler, same components, table reshaped.
#[test]
fn a_wrong_api_version_is_refused() {
    let Some(f) = setup("abi gate api version", &["api_version"]) else { return };
    let run = probe_one(&f);
    assert_eq!(run.only(), Verdict::Refused,
               "a module built against a different ENGINE_GAME_API_VERSION was \
                accepted.\nhost said:\n{}", run.stderr);
}

/// A mismatched `abiFingerprint`: the debug-module-against-release-host case.
///
/// The fixture supplies a well-formed but different string rather than null, so
/// the STRING COMPARE is what has to reject it. A null would take the
/// `!t->abiFingerprint` short-circuit and never reach the comparison — passing
/// this test while leaving the branch that actually matters unexercised.
#[test]
fn a_mismatched_abi_fingerprint_is_refused() {
    let Some(f) = setup("abi gate fingerprint", &["fingerprint"]) else { return };
    let run = probe_one(&f);
    assert_eq!(run.only(), Verdict::Refused,
               "a module whose compiler/std/build-mode fingerprint differs from \
                the host was accepted. This is the check that catches a debug \
                module in a release host, which corrupts memory in ways no \
                version integer sees.\nhost said:\n{}", run.stderr);
}

/// A changed `componentLayoutHash`: a shared component grew since the host built.
///
/// The one refusal whose remedy is "restart the host" rather than "rebuild the
/// module" — world data survives reloads, so this module would misread live ECS
/// memory that the running host is still using.
#[test]
fn a_changed_component_layout_is_refused() {
    let Some(f) = setup("abi gate layout hash", &["layout_hash"]) else { return };
    let run = probe_one(&f);
    assert_eq!(run.only(), Verdict::Refused,
               "a module built against different component layouts was accepted. \
                Live world data would be misread.\nhost said:\n{}", run.stderr);
}

/// Two modules declaring the same contract at different versions.
///
/// The only gate condition a single module cannot provoke: the registry starts
/// empty, so the first module to declare a contract PINS it and always passes.
/// The mismatch exists only in the second one, which is why the probe takes a
/// list and holds every library open until it exits.
#[test]
fn contract_version_skew_between_two_modules_is_refused() {
    let Some(f) = setup("abi gate contracts", &["contract_v1", "contract_v2"]) else { return };
    let run = probe_one(&f);
    assert!(run.completed, "probe did not finish:\n{}", run.stdout);
    assert_eq!(
        run.verdicts,
        vec![Verdict::Ok, Verdict::Refused],
        "the first module declares contract 'abi_gate' v1 and must PIN it; the \
         second declares v2 and must be refused. Getting [Ok, Ok] means two kits \
         can disagree about a shared component's layout while flecs matches them \
         by name — which is silent corruption, not a load error.\nhost said:\n{}",
        run.stderr
    );
}

// ── Not a module at all ─────────────────────────────────────────────────────

/// A shared library that loads fine and exports none of what a module must.
///
/// A mis-pathed project.json, a system library, a kit whose export macro was
/// dropped. dlopen succeeds and both symbol lookups return null, so the host has
/// to notice before it calls through one of them.
#[test]
fn a_library_without_the_module_exports_is_refused() {
    let Some(f) = setup("abi gate exports", &["no_exports"]) else { return };
    let run = probe_one(&f);
    assert_eq!(run.only(), Verdict::Refused,
               "a shared library with no engineGameModuleCreateV1 was accepted.\n\
                host said:\n{}", run.stderr);
    assert!(run.completed,
            "the probe did not survive a library with no module exports — it \
             very likely called through a null pointer:\n{}", run.stdout);
}

/// A module whose `create` decides at runtime that it cannot initialise.
///
/// There is no struct field to check here, only a null test — the kind a
/// refactor drops without noticing, because every fixture that returns a real
/// table keeps passing.
#[test]
fn a_module_whose_create_returns_null_is_refused() {
    let Some(f) = setup("abi gate null table", &["null_table"]) else { return };
    let run = probe_one(&f);
    assert_eq!(run.only(), Verdict::Refused,
               "engineGameModuleCreateV1 returned null and the host accepted it.\n\
                host said:\n{}", run.stderr);
    assert!(run.completed,
            "the probe did not survive a null table — it dereferenced it:\n{}",
            run.stdout);
}

// ── The refusal path itself ─────────────────────────────────────────────────

/// A refused module still gets `destroy` called on its table.
///
/// `refuse()` is a lambda invoked from four separate arms of the gauntlet, and
/// on a rejection it is the ONLY thing that frees the module's table and
/// instance: an arm that returns `false` on its own leaves `m_table` unset, so
/// the later `unload()` has nothing to free and the allocation is simply lost.
/// That is a leak on the unhappy path — which no other test walks, in a host
/// that retries the load on every file change. A slow drip nothing else sees.
///
/// Checked across every defect that produced a table rather than one, because
/// the whole risk is that ONE of the four arms diverges from the others.
///
/// ## What the trace can and cannot distinguish
///
/// An ACCEPTED module is also destroyed — at teardown, when `~ModuleLibrary`
/// runs — so both outcomes end up spelled `create, destroy` and the trace cannot
/// say WHEN the destroy happened. What it can say is that the counts are right,
/// and that is where both failures live: a refusal that skipped `refuse()`
/// records `create` alone, and a double-free records two `destroy`s. So this
/// asserts exactly one of each, and the accepted case is checked the same way —
/// as the control proving the pattern is not vacuously true of everything.
#[test]
fn every_module_that_builds_a_table_gets_it_destroyed_exactly_once() {
    let names = ["struct_size", "api_version", "fingerprint", "layout_hash", "good"];
    let Some(fixtures) = setup("abi gate refusal cleanup", &names) else { return };
    let dir = m::scratch("cleanup");

    for (name, path) in names.iter().zip(&fixtures) {
        let trace = dir.join(format!("{name}.trace"));
        let run = m::run(&[path.as_path()], false, Some(&trace));
        let expected = if *name == "good" { Verdict::Ok } else { Verdict::Refused };
        assert_eq!(run.only(), expected, "fixture '{name}' got the wrong verdict");

        let events: Vec<String> = m::read_trace(&trace).into_iter().map(|(_, e)| e).collect();
        assert_eq!(
            events,
            vec!["create".to_string(), "destroy".to_string()],
            "fixture '{name}' ({expected:?}) recorded {events:?}. One 'create' with \
             no 'destroy' means the table and its instance were LEAKED — for a \
             refusal, that is an arm of the gauntlet returning without calling \
             refuse(). Two 'destroy's means it was freed twice."
        );
    }
}

// ── Both load paths ─────────────────────────────────────────────────────────

/// The gate must reach the same verdict whether or not the image was copied.
///
/// `load()` has two entries: copy-to-temp for hot reload (the editor) and
/// in-place (`Reload::Never`, what a shipped player does with dist/kits/). They
/// converge on `finishLoad`, so today they cannot disagree — but the split was
/// added after the fact, to a function that used to have one path, and the
/// shipping posture is the one that runs the branch nothing else tests.
#[test]
fn both_load_paths_agree_on_every_verdict() {
    let names = ["good", "struct_size", "api_version", "fingerprint", "layout_hash", "null_table"];
    let Some(fixtures) = setup("abi gate reload paths", &names) else { return };

    for (name, path) in names.iter().zip(&fixtures) {
        let copied = m::run(&[path.as_path()], false, None);
        let in_place = m::run(&[path.as_path()], true, None);
        assert_eq!(
            copied.only(), in_place.only(),
            "fixture '{name}' got {:?} through the hot-reload copy path but {:?} \
             loaded in place. A shipped player uses the in-place path, so a gate \
             that only holds for the editor holds where it matters least.",
            copied.only(), in_place.only()
        );
    }
}

// ── The probe's own contract ────────────────────────────────────────────────

/// A path that does not exist is distinguishable from a refusal.
///
/// Every assertion above reads "refused" off the probe's stdout. If a missing
/// file also printed "refused", a suite whose fixtures had silently stopped
/// being built would pass every refusal test in this file and fail only the
/// control — which is one confusing failure standing in for nine.
#[test]
fn a_missing_module_is_not_reported_as_a_refusal() {
    if m::probe().is_none() {
        m::skip("abi gate probe contract", &m::probe_absence());
        return;
    }
    let nope = std::env::temp_dir().join("engine_module_abi_definitely_absent.so");
    let _ = std::fs::remove_file(&nope);
    let run = m::run(&[nope.as_path()], false, None);
    assert!(run.verdicts.is_empty(),
            "a nonexistent path produced a module verdict: {:?}", run.verdicts);
    assert_eq!(run.status, Some(3),
               "a missing module must exit 3, not blend in with a refusal");
}
