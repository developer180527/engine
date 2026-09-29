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

if failures:
    print(f"\n{len(failures)} failure(s)")
    sys.exit(1)
print("work_orders_test: all checks passed")
