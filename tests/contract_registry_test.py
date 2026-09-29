#!/usr/bin/env python3
"""contract_registry_test — the contract registry's checks, on a scratch tree.

engine_doctor reads docs/contracts/*.md, checks each contract's state against
evidence (header, implementations, registered tests, the five meaning
sections) and renders the Contracts chart. This points the doctor's module at a
throwaway tree and pins what each check decides — in particular the two ways
the chart could lie: a test "exercising" an implementation it only mentions in
a comment, and a test counted as run when nothing registers it with ctest.

Exits non-zero on the first failing check (a list of all failures is printed).
"""
import importlib.util
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("engine_doctor", HERE.parent / "scripts" / "engine_doctor.py")
ed = importlib.util.module_from_spec(spec)
sys.modules["engine_doctor"] = ed   # dataclasses resolve types through sys.modules
spec.loader.exec_module(ed)

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"FAIL: {msg}")


def write(root: Path, rel: str, text: str):
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


MEANING_FULL = "".join(f"## {s}\nSomething true about {s.lower()}.\n\n" for s in ed.SEMANTICS)


def contract(root: Path, name: str, front: str, body: str = MEANING_FULL):
    write(root, f"docs/contracts/{name}.md", f"---\nstatus: reference\ncontract: {name}\n{front}---\n# {name}\n\n{body}")
    return ed.parse_front_matter(root / f"docs/contracts/{name}.md")


def errors_for(root: Path, docs):
    return [f.msg for f in ed.check_contracts(ed.find_contracts(docs)) if f.level == "error"]


with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    ed.REPO = root
    ed._CTEST_TEXT = None

    write(root, "src/thing/thing.h", "struct IThing { virtual void go() = 0; };\n")
    write(root, "src/thing/real_thing.h", "class RealThing : public IThing {};\n")
    write(root, "src/thing/null_thing.h", "class NullThing : public IThing {};\n")
    write(root, "tests/thing_test.cpp", '#include "thing/null_thing.h"\n// the real thing is not tested here\n')
    write(root, "tests/orphan_test.cpp", "// builds, and nothing runs it\n")
    write(root, "tests/linked_test.cpp", "// run through add_test(NAME x COMMAND linked_test)\n")
    write(root, "tests/CMakeLists.txt",
          "engine_test(thing_test)\n"
          "add_executable(orphan_test orphan_test.cpp)\n"
          "add_test(NAME linked COMMAND linked_test)\n")

    base = ("kind: interface\nowner: src/thing\nheader: src/thing/thing.h\n"
            "implementations:\n  - real: src/thing/real_thing.h\n  - null: src/thing/null_thing.h\n")

    # ── 1. A complete frozen contract passes ──────────────────────────────
    good = contract(root, "good", base + "state: frozen\ntests:\n  - tests/thing_test.cpp\n")
    check(errors_for(root, [good]) == [], f"a complete frozen contract has no errors: {errors_for(root, [good])}")

    # ── 2. frozen demands a null, a registered test and all five sections ─
    no_null = contract(root, "nonull", base.replace("  - null: src/thing/null_thing.h\n", "")
                       + "state: frozen\ntests:\n  - tests/thing_test.cpp\n")
    check(any("requires a `null:`" in e for e in errors_for(root, [no_null])), "frozen without a null is refused")
    no_test = contract(root, "notest", base + "state: frozen\n")
    check(any("registered test" in e for e in errors_for(root, [no_test])), "frozen without a test is refused")
    unwritten = MEANING_FULL.replace("Something true about threading.", "Not yet written.")
    half = contract(root, "half", base + "state: frozen\ntests:\n  - tests/thing_test.cpp\n", unwritten)
    errs = errors_for(root, [half])
    check(any("missing: Threading" in e for e in errs), f"'Not yet written.' counts as missing: {errs}")
    none_null = contract(root, "nonenull", base.replace("null: src/thing/null_thing.h", "null: none")
                         + "state: frozen\ntests:\n  - tests/thing_test.cpp\n")
    check(errors_for(root, [none_null]) == [], "`null: none` (explained in the body) satisfies frozen")

    # ── 3. provisional needs the header and an implementation; planned does not
    no_header = contract(root, "nohdr", base.replace("src/thing/thing.h", "src/thing/missing.h") + "state: provisional\n")
    check(any("requires the header" in e for e in errors_for(root, [no_header])), "provisional without its header is refused")
    planned = contract(root, "planned", "kind: interface\nowner: src/thing\nheader: src/thing/future.h\n"
                       "implementations:\n  - real: src/thing/real_thing.h\nstate: planned\n")
    check(errors_for(root, [planned]) == [], "a planned contract may name a header that does not exist yet")

    # ── 4. Tests must be REGISTERED, not just exist ──────────────────────
    ed._CTEST_TEXT = None
    check(ed.test_registered("tests/thing_test.cpp"), "engine_test(name) registers")
    check(ed.test_registered("tests/linked_test.cpp"), "add_test(NAME x COMMAND stem) registers")
    check(not ed.test_registered("tests/orphan_test.cpp"), "add_executable alone does NOT register")
    orphan = contract(root, "orphan", base + "state: provisional\ntests:\n  - tests/orphan_test.cpp\n")
    check(any("not registered" in e for e in errors_for(root, [orphan])), "an unregistered test is an error")

    # ── 5. 'Exercised' needs an include or a symbol, not a word ─────────
    real_h = ed.Impl("real", "src/thing/real_thing.h")
    null_h = ed.Impl("null", "src/thing/null_thing.h")
    check(ed.impl_exercised(null_h, ["tests/thing_test.cpp"]), "an #include of the implementation counts")
    check(not ed.impl_exercised(real_h, ["tests/thing_test.cpp"]),
          "'the real thing' in a comment does not count as exercising real_thing.h")
    write(root, "tests/sym_test.cpp", "RealThing t; // uses the class\n")
    check(ed.impl_exercised(ed.Impl("real", "src/thing/real_thing.h", "RealThing"), ["tests/sym_test.cpp"]),
          "naming the #Symbol counts")
    check(not ed.impl_exercised(ed.Impl("real", "src/thing/real_thing.h", "Real"), ["tests/sym_test.cpp"]),
          "a symbol matches whole words only (Real is not RealThing)")

    # ── 6. Structural errors ──────────────────────────────────────────────
    bad_sym = contract(root, "badsym", base.replace("real_thing.h", "real_thing.h#Nope") + "state: provisional\n")
    check(any("`Nope` not found" in e for e in errors_for(root, [bad_sym])), "a #Symbol absent from its file is an error")
    dup_a = contract(root, "dupa", base + "state: provisional\n")
    dup_b = contract(root, "dupb", base + "state: provisional\n")
    dup_b.meta["contract"] = "dupa"
    check(any("also declared" in e for e in errors_for(root, [dup_a, dup_b])), "two files cannot declare one contract")
    weird = contract(root, "weird", base.replace("kind: interface", "kind: vibes") + "state: provisional\n")
    check(any("unknown contract kind" in e for e in errors_for(root, [weird])), "unknown kinds are refused")

if failures:
    print(f"\ncontract_registry_test: {len(failures)} FAILURE(S)")
    sys.exit(1)
print("contract_registry_test: all checks passed")
