#!/usr/bin/env python3
"""work_orders_test — every rule in scripts/work_orders.py, on a scratch tree.

Starts from a small VALID tree (which must pass), then breaks it one rule at a
time and asserts the checker names that rule. A checker that passes the real
tree proves nothing on its own; this is what shows each rule can fail.

Exits non-zero on the first run with any failure (all failures are printed).
"""
import importlib.util
import io
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("work_orders", HERE.parent / "scripts" / "work_orders.py")
wo = importlib.util.module_from_spec(spec)
sys.modules["work_orders"] = wo
spec.loader.exec_module(wo)

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print(f"FAIL: {msg}")


def order(id_, *, state="todo", priority="P2", size="M", depends="", contracts="",
          new_contracts="", touches="src/a.cpp", extra="", why="Because.",
          done_when="- [ ] a thing", contract_sec=None):
    fm = [f"status: plan", f"id: {id_}", f"title: Order {id_}", "program: assets",
          f"priority: {priority}", f"size: {size}", f"state: {state}"]
    if depends: fm.append(f"depends: [{depends}]")
    if contracts: fm.append(f"contracts: [{contracts}]")
    if new_contracts: fm.append(f"new-contracts: [{new_contracts}]")
    if touches: fm += ["touches:", f"  - {touches}"]
    if extra: fm.append(extra)
    body = f"## Why\n{why}\n\n## Done when\n{done_when}\n"
    if contract_sec is not None:
        body += f"\n## Contract\n{contract_sec}\n"
    return "---\n" + "\n".join(fm) + "\n---\n" + body


def base():
    return {
        "WO-001-a.md": order("WO-001", state="done", done_when="- [x] did it",
                             extra="done: 2026-09-01\nevidence: abc123"),
        "WO-002-b.md": order("WO-002", depends="WO-001", contracts="cooker",
                             contract_sec="Nothing: a stub that says so."),
        "WO-003-c.md": order("WO-003", depends="WO-002", priority="P1"),
        "WO-004-d.md": order("WO-004", state="parked", size="XL",
                             extra="parked-until: later"),
    }


def run(files, argv=("check",)):
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        (root / "docs/work").mkdir(parents=True)
        (root / "docs/contracts").mkdir(parents=True)
        (root / "src").mkdir()
        (root / "src/a.cpp").write_text("")
        (root / "docs/contracts/cooker.md").write_text("---\ncontract: cooker\n---\n")
        for name, text in files.items():
            (root / "docs/work" / name).write_text(text)
        out = io.StringIO()
        with redirect_stdout(out):
            rc = wo.main(["--root", str(root), *argv])
        board = (root / "docs/work/BOARD.md")
        return rc, out.getvalue(), (board.read_text() if board.exists() else "")


def rejects(desc, files, needle):
    rc, out, _ = run(files)
    check(rc == 1 and needle in out, f"{desc}: expected rejection mentioning {needle!r}, got rc={rc}:\n{out}")


def with_(name, text):
    f = base(); f[name] = text; return f


# ── The valid tree passes ────────────────────────────────────────────────────
rc, out, _ = run(base())
check(rc == 0, f"valid tree rejected:\n{out}")

# ── Each rule, broken on its own ─────────────────────────────────────────────
rejects("missing key", with_("WO-003-c.md", order("WO-003").replace("size: M\n", "")), "missing `size:`")
rejects("bad status", with_("WO-003-c.md", order("WO-003").replace("status: plan", "status: as-built")), "`status:` must be `plan`")
rejects("bad id", with_("WO-003-c.md", order("WO-3")), "not WO-NNN")
rejects("filename/id mismatch", with_("WO-003-c.md", order("WO-009")), "filename must start with its id")
rejects("duplicate id", with_("WO-005-e.md", order("WO-005")) | {"WO-005-f.md": order("WO-005")}, "duplicate id")
rejects("bad priority", with_("WO-003-c.md", order("WO-003", priority="P9")), "priority `P9`")
rejects("bad size", with_("WO-003-c.md", order("WO-003", size="XXL")), "size `XXL`")
rejects("bad state", with_("WO-003-c.md", order("WO-003", state="doing")), "state `doing`")
rejects("XL not parked", with_("WO-003-c.md", order("WO-003", size="XL")), "size XL may only be parked")
rejects("parked without until", with_("WO-004-d.md", order("WO-004", state="parked")), "parked-until")
rejects("P0 parked", with_("WO-004-d.md", order("WO-004", state="parked", priority="P0", extra="parked-until: x")), "P0 cannot be parked")
rejects("no Why", with_("WO-003-c.md", order("WO-003", why="")), "missing `## Why`")
rejects("no checkboxes", with_("WO-003-c.md", order("WO-003", done_when="just prose")), "no `- [ ]` items")
rejects("done with open box", with_("WO-001-a.md", order("WO-001", state="done", done_when="- [x] a\n- [ ] b",
        extra="done: 2026-09-01\nevidence: x")), "1 `Done when` item(s) unticked")
rejects("done without date", with_("WO-001-a.md", order("WO-001", state="done", done_when="- [x] a",
        extra="evidence: x")), "done: YYYY-MM-DD")
rejects("done without evidence", with_("WO-001-a.md", order("WO-001", state="done", done_when="- [x] a",
        extra="done: 2026-09-01")), "evidence:")
# The contract rule.
rejects("contract, no section", with_("WO-002-b.md", order("WO-002", depends="WO-001", contracts="cooker")),
        "no `## Contract` section")
rejects("contract, no Nothing line", with_("WO-002-b.md", order("WO-002", depends="WO-001", contracts="cooker",
        contract_sec="It is a contract.")), "needs a `Nothing:` line")
rejects("unknown contract", with_("WO-002-b.md", order("WO-002", depends="WO-001", contracts="nope",
        contract_sec="Nothing: x")), "contract `nope` is not in")
rejects("new contract already registered", with_("WO-002-b.md", order("WO-002", depends="WO-001",
        new_contracts="cooker", contract_sec="Nothing: x")), "already registered")
rc, out, _ = run(with_("WO-002-b.md", order("WO-002", depends="WO-001", new_contracts="fresh",
                 contract_sec="- **Nothing**: a job, pending until cooked.")))
check(rc == 0, f"new contract with a bold `Nothing:` bullet should pass:\n{out}")
rejects("touches missing path", with_("WO-003-c.md", order("WO-003", touches="src/gone.cpp")), "does not exist: src/gone.cpp")
rc, out, _ = run(with_("WO-001-a.md", order("WO-001", state="done", done_when="- [x] a", touches="src/gone.cpp",
                 extra="done: 2026-09-01\nevidence: x")))
check(rc == 0, f"a DONE order may point at a path that has since moved:\n{out}")
# Graph.
rejects("unknown dependency", with_("WO-003-c.md", order("WO-003", depends="WO-099")), "unknown WO-099")
rejects("active while blocked", with_("WO-003-c.md", order("WO-003", depends="WO-002", state="active")),
        "active but its dependency WO-002 is todo")
rejects("cycle", with_("WO-002-b.md", order("WO-002", depends="WO-003", contracts="cooker",
        contract_sec="Nothing: x")), "dependency cycle")
f = base()
for i in (5, 6, 7):
    f[f"WO-00{i}-x.md"] = order(f"WO-00{i}", state="active")
rejects("WIP limit", f, "3 orders active")

# Titles: a value that STARTS with a quote but is not wrapped in quotes keeps it.
f = with_("WO-003-c.md", order("WO-003").replace("title: Order WO-003", 'title: "Reveal" works'))
f["WO-006-q.md"] = order("WO-006").replace("title: Order WO-006", 'title: "`os::` layer"')
rc, _, board = run(f, ("board",))
check('| "Reveal" works |' in board, "a title starting with a quote must keep both quotes:\n" + board)
check("| `os::` layer |" in board, "a fully quoted title must be unwrapped once")

# ── The board is derived, and --check catches drift ──────────────────────────
rc, _, board = run(base(), ("board",))
check(rc == 0, "board generation failed")
check("| [WO-003](WO-003-c.md) | Order WO-003 | Asset import & cooking | M | blocked by WO-002 |" in board,
      "WO-003 should be derived as blocked by WO-002:\n" + board)
check("1. [WO-002](WO-002-b.md)" in board, "WO-002 (deps done) should be first under Next up")
check("## Parked" in board and "later" in board, "parked order and its until-condition missing from board")
check("| [WO-001](WO-001-a.md) | Order WO-001 | 2026-09-01 | abc123 |" in board, "done row missing")
f = base(); f["WO-005-x.md"] = order("WO-005", state="active", done_when="- [x] one\n- [ ] two")
rc, _, board = run(f, ("board",))
check("2/1" not in board and "1/2 done. Still open:" in board and "  - two" in board,
      "active order should show progress and its open items:\n" + board)

with tempfile.TemporaryDirectory() as t:
    pass  # --check needs a persistent tree; exercised below via main directly
def board_check_roundtrip():
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        (root / "docs/work").mkdir(parents=True); (root / "docs/contracts").mkdir(parents=True)
        (root / "src").mkdir(); (root / "src/a.cpp").write_text("")
        (root / "docs/contracts/cooker.md").write_text("---\ncontract: cooker\n---\n")
        for n, txt in base().items(): (root / "docs/work" / n).write_text(txt)
        q = io.StringIO()
        with redirect_stdout(q):
            fresh_missing = wo.main(["--root", str(root), "board", "--check"])
            wo.main(["--root", str(root), "board"])
            fresh = wo.main(["--root", str(root), "board", "--check"])
            (root / "docs/work/WO-003-c.md").write_text(order("WO-003", depends="WO-002", priority="P0"))
            stale = wo.main(["--root", str(root), "board", "--check"])
        check(fresh_missing == 1, "board --check must fail when BOARD.md does not exist")
        check(fresh == 0, "board --check must pass right after regeneration")
        check(stale == 1, "board --check must fail after an order changes")
board_check_roundtrip()

# ── next ─────────────────────────────────────────────────────────────────────
rc, out, _ = run(base(), ("next",))
check(rc == 0 and "NEXT UP" in out and "WO-002" in out and "WO-003" not in out.split("NEXT UP")[1].split("\n\n")[0],
      "next should list WO-002 and not the blocked WO-003:\n" + out)

# ── brief: each parser, then the command on a scratch tree and a real git repo ─
check(wo.age(30) == "just now" and wo.age(90) == "1 minute ago" and wo.age(7200) == "2 hours ago"
      and wo.age(86400 * 3) == "3 days ago", "age() wording")
check(wo.area_of("src/scene/unresolved_mesh.h") == "src/scene" and wo.area_of("tests/x.cpp") == "tests"
      and wo.area_of("ENGINE_STATUS.md") == "(repo root)", "area_of: two levels under src/, one elsewhere")
g = wo.group_status(" M src/scene/a.h\n M src/scene/b.h\n?? tests/t.cpp\nR  src/old.h -> src/render/new.h\n")
check(g == {"src/scene": 2, "src/render": 1, "tests": 1}, f"group_status counts by area, renames by destination: {g}")

status_md = """# Engine Status
Generated 2026-09-30 from commit `abc1234`.
## Summary
- **Stale docs:** 2
## ⚠️ Stale — code moved after the doc was last verified

- `src/project` — code 2026-08-25, verified 2026-08-01
- `src/editor` — code 2026-09-30, verified 2026-08-18

## Unreviewed docs
- `README.md`
"""
items, commit = wo.stale_docs(status_md)
check(commit == "abc1234" and items == ["`src/project` — code 2026-08-25, verified 2026-08-01",
      "`src/editor` — code 2026-09-30, verified 2026-08-18"],
      f"stale_docs reads only the Stale section, not Summary or Unreviewed: {items} @ {commit}")

oq_md = """# Open
---
- **Resize redraw on Wayland.** Nothing redraws while dragging.
  - **where** src/runtime/platform/
- **Cook determinism.** `src/assets/cookers/mesh/mesh_cooker.cpp` can differ per run.
- **Unrelated thing.** `src/runtime_extra/x.h` only looks similar.
## Another section
- **Not a question.** after a heading, still parsed as a new item
"""
qs = wo.open_questions(oq_md)
check([q["title"] for q in qs] == ["Resize redraw on Wayland", "Cook determinism", "Unrelated thing",
      "Not a question"], f"open_questions titles: {[q['title'] for q in qs]}")
check(qs[0]["where"] == ["src/runtime/platform/"], "the where trailer is read")
check(wo.overlapping(qs, ["src/runtime/platform/glfw_platform.cpp"]) == ["Resize redraw on Wayland"],
      "a file under a question's `where` directory matches")
check(wo.overlapping(qs, ["src/assets/cookers/mesh/mesh_cooker.cpp"]) == ["Cook determinism"],
      "a path named in the question's text matches")
check(wo.overlapping(qs, ["src/runtime"]) == ["Resize redraw on Wayland"],
      "a directory touch matches questions under it, and src/runtime is NOT a prefix of src/runtime_extra")
check(wo.overlapping(qs, ["scripts/work_orders.py"]) == [], "no false matches")

# A partial ctest run must say it is partial.
partial = "Start testing\n106/120 Testing: docs_contract\n107/120 Testing: docs_status_current\n" \
          "Label Time Summary:\ndocs =   4.10 sec*proc (2 tests)\n"
d = wo.describe_run(partial, [])
check(d == "2 of 120 tests (labels: docs) — a PARTIAL run, all passed", f"partial run described: {d}")
full = "".join(f"{i}/3 Testing: t{i}\n" for i in (1, 2, 3)) + "unit =   1.0 sec*proc\n"
d = wo.describe_run(full, ["t2"])
check(d == "3 of 3 tests (labels: unit), 1 failed — t2", f"full run with a failure described: {d}")

# The command itself, outside a repo: runs, says so, never crashes.
rc, out, _ = run(base(), ("brief",))
check(rc == 0 and "not a git repository" in out and "NEXT UP" in out and "read-only" in out,
      "brief outside a git repo still prints the queue:\n" + out)
# With a broken order it still runs — that is when you need it — and shows the errors.
rc, out, _ = run(with_("WO-003-c.md", order("WO-003", size="XXL")), ("brief",))
check(rc == 1 and "WHERE YOU ARE" in out and "ERROR(S)" in out and "size `XXL`" in out,
      "brief with a broken order prints the errors instead of the queue:\n" + out)

# In a real git repo: last commit, commits since a ref, uncommitted by area.
import shutil, subprocess, time as _t
if shutil.which("git"):
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        def sh(*a): subprocess.run(["git", "-C", t, *a], check=True, capture_output=True)
        sh("init", "-q"); sh("config", "user.email", "t@example.invalid"); sh("config", "user.name", "T")
        (root / "docs/work").mkdir(parents=True); (root / "docs/contracts").mkdir(parents=True)
        (root / "src").mkdir(); (root / "src/a.cpp").write_text("")
        (root / "docs/contracts/cooker.md").write_text("---\ncontract: cooker\n---\n")
        for n, txt in base().items(): (root / "docs/work" / n).write_text(txt)
        sh("add", "-A"); sh("commit", "-q", "-m", "first")
        (root / "src/b.cpp").write_text(""); sh("add", "-A"); sh("commit", "-q", "-m", "second change")
        (root / "src/scene").mkdir(); (root / "src/scene/c.h").write_text("")
        q = io.StringIO()
        with redirect_stdout(q):
            rc = wo.main(["--root", t, "brief", "--since", "HEAD~1"])
        out = q.getvalue()
        check(rc == 0 and "second change" in out.split("since HEAD~1")[0], "last commit shown:\n" + out)
        check("since HEAD~1: 1 commit(s)" in out, "commits since the given ref")
        check("uncommitted: 1 file(s) — src/scene 1" in out or "uncommitted: 1 file(s) — src 1" in out,
              "uncommitted grouped by area:\n" + out)

# And the real repo, against the order's budget: under 2 s, read-only. The
# budget is a promise about a developer's machine, where brief is how you come
# back to the project. On a CI runner (GitHub sets CI=true) the first git call
# stats every file of 14 submodules on a cold disk, so a wall clock there
# measures the runner: 3.15 s on macOS CI with brief itself at 0.1 s locally
# (WO-038). CI still runs it, and prints the time.
import os as _os
q = io.StringIO(); t0 = _t.monotonic()
with redirect_stdout(q):
    wo.main(["brief"])
elapsed = _t.monotonic() - t0
if _os.environ.get("CI") == "true":
    print(f"brief on the real repo: {elapsed:.2f}s (budget 2s applies off CI)")
else:
    check(elapsed < 2.0, f"brief on the real repo took {elapsed:.2f}s (budget 2s)")

# ── Decision records (WO-022) ────────────────────────────────────────────────
def record(id_="DR-0001", *, status="decided", source="docs/plans/p.md", extra="",
           sections=("Decided", "Rejected", "Why", "What would reverse it"), empty=None):
    fm = [f"status: {status}", f"id: {id_}", "title: A decision", "date: 2026-10-01"]
    if source: fm += ["source:", f"  - {source}"]
    if extra: fm.append(extra)
    body = "".join(f"\n## {x}\n{'' if x == empty else 'Text.'}\n" for x in sections)
    return "---\n" + "\n".join(fm) + "\n---\n" + body


def run_dr(records, docs, argv=("check",)):
    """The valid work tree plus decision records and arbitrary docs."""
    with tempfile.TemporaryDirectory() as t:
        root = Path(t)
        (root / "docs/work").mkdir(parents=True)
        (root / "docs/contracts").mkdir(parents=True)
        (root / "docs/process/decisions").mkdir(parents=True)
        (root / "src").mkdir()
        (root / "src/a.cpp").write_text("")
        (root / "docs/contracts/cooker.md").write_text("---\ncontract: cooker\n---\n")
        for name, text in base().items():
            (root / "docs/work" / name).write_text(text)
        for name, text in records.items():
            (root / "docs/process/decisions" / name).write_text(text)
        for rel, text in docs.items():
            (root / rel).parent.mkdir(parents=True, exist_ok=True)
            (root / rel).write_text(text)
        out = io.StringIO()
        with redirect_stdout(out):
            rc = wo.main(["--root", str(root), *argv])
        idx = root / "docs/process/decisions/README.md"
        return rc, out.getvalue(), (idx.read_text() if idx.exists() else "")


GOOD_DOC = {"docs/plans/p.md": "We pick X over Y (DR-0001).\n"}
rc, out, _ = run_dr({"DR-0001-a.md": record()}, GOOD_DOC)
check(rc == 0 and "1 decision records" in out, f"valid decision tree rejected:\n{out}")


def dr_rejects(desc, records, docs, needle):
    rc, out, _ = run_dr(records, docs)
    check(rc == 1 and needle in out, f"{desc}: expected rejection mentioning {needle!r}, got rc={rc}:\n{out}")


dr_rejects("bad id", {"DR-0001-a.md": record("DR-1")}, GOOD_DOC, "not DR-NNNN")
dr_rejects("filename/id", {"DR-0001-a.md": record("DR-0002")}, {"docs/plans/p.md": "DR-0002"}, "filename must start")
dr_rejects("bad status", {"DR-0001-a.md": record(status="maybe")}, GOOD_DOC, "`status:` must be one of")
dr_rejects("a section missing", {"DR-0001-a.md": record(sections=("Decided", "Why", "What would reverse it"))},
           GOOD_DOC, "sections must be exactly")
dr_rejects("sections out of order", {"DR-0001-a.md": record(sections=("Why", "Decided", "Rejected", "What would reverse it"))},
           GOOD_DOC, "sections must be exactly")
dr_rejects("an empty section", {"DR-0001-a.md": record(empty="Rejected")}, GOOD_DOC, "`## Rejected` is empty")
dr_rejects("no source", {"DR-0001-a.md": record(source="")}, GOOD_DOC, "missing `source:`")
dr_rejects("source missing", {"DR-0001-a.md": record(source="docs/plans/gone.md")}, GOOD_DOC, "does not exist")
dr_rejects("source does not cite it (the back-link)", {"DR-0001-a.md": record()},
           {"docs/plans/p.md": "We pick X over Y.\n"}, "does not cite DR-0001")
dr_rejects("a dangling citation", {"DR-0001-a.md": record()},
           {"docs/plans/p.md": "DR-0001\n", "src/x.h": "// see DR-0042\n"}, "cites DR-0042, which is not")
dr_rejects("superseded without successor", {"DR-0001-a.md": record(status="superseded")}, GOOD_DOC, "superseded-by")
dr_rejects("superseded by nothing", {"DR-0001-a.md": record(status="superseded", extra="superseded-by: DR-0009")},
           GOOD_DOC, "is not a decision record")
dr_rejects("\"we decided\" with no record", {"DR-0001-a.md": record()},
           {"docs/plans/p.md": "DR-0001\n\nHere we decided to keep it.\n"}, "says \"we decided\" without citing")
rc, out, _ = run_dr({"DR-0001-a.md": record()},
                    {"docs/plans/p.md": "DR-0001\n\nWe decided to keep it (DR-0001).\n"})
check(rc == 0, f"\"we decided\" WITH a citation in the paragraph passes:\n{out}")
# The index: generated, and --check catches a stale one.
rc, out, idx = run_dr({"DR-0001-a.md": record()}, GOOD_DOC, ("decisions",))
check(rc == 0 and "[DR-0001](DR-0001-a.md)" in idx and "GENERATED" in idx, f"index written:\n{out}\n{idx}")
rc, out, _ = run_dr({"DR-0001-a.md": record()}, GOOD_DOC, ("decisions", "--check"))
check(rc == 1 and "out of date" in out, f"a missing index fails --check:\n{out}")

if failures:
    print(f"\n{len(failures)} failure(s)")
    sys.exit(1)
print("work_orders_test: all checks passed")
