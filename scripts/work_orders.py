#!/usr/bin/env python3
"""work_orders — the engine's work queue, one file per order, checked.

    python3 scripts/work_orders.py brief          back after a break? start here
    python3 scripts/work_orders.py next           what to do now
    python3 scripts/work_orders.py check          validate every order; exit 1 on error
    python3 scripts/work_orders.py board          regenerate docs/work/BOARD.md
    python3 scripts/work_orders.py board --check  fail if BOARD.md is out of date (CI)

Orders live in docs/work/WO-NNN-slug.md. The format and the rules are in
docs/work/README.md. Everything on the board is DERIVED from the order files —
"ready" vs "blocked" is computed from dependencies, progress from the
`## Done when` checkboxes — so the board cannot drift from the orders, and an
order cannot claim a state its own file contradicts.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from datetime import datetime
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORK_DIR = "docs/work"
CONTRACT_DIR = "docs/contracts"
BOARD = "BOARD.md"

PRIORITIES = {
    "P0": "broken now — wrong output or lost data. Nothing else starts first.",
    "P1": "cheap, and makes everything after it cheaper (process, gates, small fixes).",
    "P2": "the planned programmes, in dependency order.",
    "P3": "later. Real, but nothing is waiting on it.",
}
SIZES = {"S": "one session", "M": "2-4 sessions", "L": "a staged week or more",
         "XL": "too big to start — split it (allowed only while parked)"}
STATES = ("todo", "active", "done", "parked")
PROGRAMS = {
    "process":     "Process & context",
    "assets":      "Asset import & cooking",
    "portability": "Portability",
    "renderer":    "Renderer & RHI",
    "providers":   "Providers & modules",
}
WIP_LIMIT = 2   # active orders at once. Half-finished work is what costs context.

REQUIRED = ("status", "id", "title", "program", "priority", "size", "state")
ID_RE = re.compile(r"^WO-\d{3}$")
BOX_RE = re.compile(r"^\s*- \[( |x|X)\] ")


@dataclass
class Order:
    path: Path
    meta: dict
    body: str
    sections: dict = field(default_factory=dict)

    @property
    def id(self) -> str: return str(self.meta.get("id", ""))
    @property
    def state(self) -> str: return str(self.meta.get("state", ""))
    @property
    def priority(self) -> str: return str(self.meta.get("priority", ""))
    def lst(self, key) -> list: 
        v = self.meta.get(key, [])
        return v if isinstance(v, list) else ([v] if v else [])

    def boxes(self) -> tuple[int, int]:
        done = total = 0
        for line in self.sections.get("Done when", "").splitlines():
            m = BOX_RE.match(line)
            if m:
                total += 1
                done += m.group(1) in "xX"
        return done, total

    def open_items(self) -> list[str]:
        return [BOX_RE.sub("", l).strip()
                for l in self.sections.get("Done when", "").splitlines()
                if BOX_RE.match(l) and "[ ]" in l]


# ── Parsing ──────────────────────────────────────────────────────────────────
# A deliberately small YAML subset — `key: value`, `key: [a, b]`, and `- item`
# lists — so this has no dependency and no surprises.
def parse(path: Path) -> Order | str:
    text = path.read_text(encoding="utf-8")
    if not text.startswith("---\n"):
        return "no front-matter"
    end = text.find("\n---\n", 4)
    if end < 0:
        return "front-matter never closes"
    meta: dict = {}
    key = None
    for raw in text[4:end].splitlines():
        line = raw.split(" #", 1)[0].rstrip()
        if not line.strip():
            continue
        m = re.match(r"^\s+- (.*)$", line)
        if m and key:
            meta.setdefault(key, [])
            if not isinstance(meta[key], list):
                return f"`{key}:` mixes a value and a list"
            meta[key].append(m.group(1).strip())
            continue
        m = re.match(r"^([a-z][a-z0-9-]*):\s*(.*)$", line)
        if not m:
            return f"cannot parse front-matter line: {raw!r}"
        key, val = m.group(1), m.group(2).strip()
        if val.startswith("[") and val.endswith("]"):
            meta[key] = [v.strip() for v in val[1:-1].split(",") if v.strip()]
        elif len(val) >= 2 and val[0] == val[-1] == '"':
            meta[key] = val[1:-1]        # quoted: unwrap once, keep inner quotes
        elif val:
            meta[key] = val
        else:
            meta[key] = []
    body = text[end + 5:]
    sections, name, buf = {}, None, []
    for line in body.splitlines():
        m = re.match(r"^## (.+?)\s*$", line)
        if m:
            if name: sections[name] = "\n".join(buf).strip()
            name, buf = m.group(1), []
        elif name:
            buf.append(line)
    if name: sections[name] = "\n".join(buf).strip()
    return Order(path, meta, body, sections)


def load(root: Path) -> tuple[list[Order], list[str]]:
    orders, errs = [], []
    for p in sorted((root / WORK_DIR).glob("WO-*.md")):
        o = parse(p)
        if isinstance(o, str):
            errs.append(f"{p.name}: {o}")
        else:
            orders.append(o)
    return orders, errs


def known_contracts(root: Path) -> set[str]:
    names = set()
    for p in (root / CONTRACT_DIR).glob("*.md"):
        m = re.search(r"^contract:\s*(\S+)", p.read_text(encoding="utf-8"), re.M)
        if m: names.add(m.group(1))
    return names


# ── Rules ────────────────────────────────────────────────────────────────────
def check(root: Path, orders: list[Order]) -> list[str]:
    errs: list[str] = []
    by_id: dict[str, Order] = {}
    contracts = known_contracts(root)

    for o in orders:
        n = o.path.name
        def e(msg): errs.append(f"{n}: {msg}")
        for k in REQUIRED:
            if not o.meta.get(k):
                e(f"missing `{k}:`")
        if o.meta.get("status") and o.meta["status"] != "plan":
            e("`status:` must be `plan` (the doc contract's kind for backlog)")
        if not ID_RE.match(o.id):
            e(f"id `{o.id}` is not WO-NNN"); continue
        if not n.startswith(o.id + "-"):
            e(f"filename must start with its id `{o.id}-`")
        if o.id in by_id:
            e(f"duplicate id {o.id} (also {by_id[o.id].path.name})")
        by_id[o.id] = o
        if o.priority and o.priority not in PRIORITIES:
            e(f"priority `{o.priority}` (want {'/'.join(PRIORITIES)})")
        if o.meta.get("program") and o.meta["program"] not in PROGRAMS:
            e(f"program `{o.meta['program']}` (want {'/'.join(PROGRAMS)})")
        size = o.meta.get("size", "")
        if size and size not in SIZES:
            e(f"size `{size}` (want {'/'.join(SIZES)})")
        if o.state and o.state not in STATES:
            e(f"state `{o.state}` (want {'/'.join(STATES)})")

        # Too big to start is too big to hold in your head.
        if size == "XL" and o.state != "parked":
            e("size XL may only be parked — split it into orders of L or smaller")
        if o.state == "parked" and not o.meta.get("parked-until"):
            e("parked with no `parked-until:` — say what would unpark it")
        if o.priority == "P0" and o.state == "parked":
            e("a P0 cannot be parked — it is broken now")

        for sec in ("Why", "Done when"):
            if not o.sections.get(sec):
                e(f"missing `## {sec}`")
        done, total = o.boxes()
        if o.sections.get("Done when") and total == 0:
            e("`## Done when` has no `- [ ]` items — done must be checkable")
        if o.state == "done":
            if done != total:
                e(f"state done but {total - done} `Done when` item(s) unticked")
            if not o.meta.get("done"):
                e("state done needs `done: YYYY-MM-DD`")
            if not o.meta.get("evidence"):
                e("state done needs `evidence:` (commit, test, or measurement)")

        # The contract rule: say what callers get before the real thing exists.
        named = o.lst("contracts") + o.lst("new-contracts")
        for c in o.lst("contracts"):
            if contracts and c not in contracts:
                e(f"contract `{c}` is not in {CONTRACT_DIR}/ "
                  "(list it under `new-contracts:` if this order creates it)")
        for c in o.lst("new-contracts"):
            if c in contracts and o.state != "done":
                e(f"`{c}` is already registered — move it to `contracts:`")
        if named:
            sec = o.sections.get("Contract", "")
            if not sec:
                e("names a contract but has no `## Contract` section")
            elif not re.search(r"^\s*(- )?\**Nothing\**:", sec, re.M):
                e("`## Contract` needs a `Nothing:` line — what a caller gets "
                  "before/without the real implementation (null, stub, fake, "
                  "or a pending job)")

        # Paths an unfinished order points at must still exist: the cheapest
        # drift signal there is (the same rule as `covers:` in the doc contract).
        if o.state != "done":
            for t in o.lst("touches"):
                if not (root / t).exists():
                    e(f"`touches:` path does not exist: {t}")

    # Graph rules.
    for o in orders:
        for d in o.lst("depends"):
            if d not in by_id:
                errs.append(f"{o.path.name}: depends on unknown {d}")
            elif o.state in ("active", "done") and by_id[d].state != "done":
                errs.append(f"{o.path.name}: {o.state} but its dependency "
                            f"{d} is {by_id[d].state}")
    seen, stack = set(), set()
    def visit(i):
        if i in stack: return i
        if i in seen or i not in by_id: return None
        stack.add(i)
        for d in by_id[i].lst("depends"):
            c = visit(d)
            if c: return c
        stack.discard(i); seen.add(i)
        return None
    for i in by_id:
        c = visit(i)
        if c:
            errs.append(f"dependency cycle through {c}"); break

    active = [o.id for o in orders if o.state == "active"]
    if len(active) > WIP_LIMIT:
        errs.append(f"{len(active)} orders active ({', '.join(active)}); the limit "
                    f"is {WIP_LIMIT} — finish or park one before starting another")
    return errs


# ── Board ────────────────────────────────────────────────────────────────────
def status_of(o: Order, by_id: dict) -> str:
    if o.state == "done": return "done"
    if o.state == "parked": return "parked"
    blockers = [d for d in o.lst("depends") if d in by_id and by_id[d].state != "done"]
    done, total = o.boxes()
    if o.state == "active": return f"**active** {done}/{total}"
    if blockers: return "blocked by " + ", ".join(blockers)
    return "**ready**"


def sort_key(o: Order):
    return (o.priority, o.id)


def ready(orders, by_id):
    return sorted([o for o in orders if o.state == "todo"
                   and status_of(o, by_id) == "**ready**"], key=sort_key)


def first_line(o: Order, sec="Why") -> str:
    for line in o.sections.get(sec, "").splitlines():
        if line.strip():
            return line.strip()
    return ""


def render(orders: list[Order]) -> str:
    by_id = {o.id: o for o in orders}
    link = lambda o: f"[{o.id}]({o.path.name})"
    out = ["---", "status: plan", "---",
           "# Work board", "",
           "<!-- GENERATED by scripts/work_orders.py — do not edit by hand.",
           "     Edit the WO-*.md files, then: python3 scripts/work_orders.py board -->",
           "",
           "What to work on, in order. Every line is derived from the order files in",
           "this directory; how they work is in [README.md](README.md). In a terminal,",
           "`python3 scripts/work_orders.py next` prints the same thing, shorter.", ""]

    act = sorted([o for o in orders if o.state == "active"], key=sort_key)
    out += ["## In progress", ""]
    if not act:
        out += ["Nothing. Start the first order under **Next up**.", ""]
    for o in act:
        d, t = o.boxes()
        out.append(f"- {link(o)} **{o.meta['title']}** — {d}/{t} done. Still open:")
        out += [f"  - {i}" for i in o.open_items()] or ["  - (all ticked: close it)"]
    out.append("")

    out += ["## Next up", ""]
    nxt = ready(orders, by_id)[:3]
    if not nxt:
        out += ["Nothing is ready: everything open is blocked or parked.", ""]
    for n, o in enumerate(nxt, 1):
        out.append(f"{n}. {link(o)} **{o.meta['title']}** · {o.priority} · "
                   f"size {o.meta['size']} — {first_line(o)}")
    out.append("")

    for p, meaning in PRIORITIES.items():
        rows = sorted([o for o in orders if o.priority == p and o.state not in ("done", "parked")],
                      key=sort_key)
        if not rows: continue
        out += [f"## {p} — {meaning}", "",
                "| order | title | area | size | status | contracts |",
                "|---|---|---|---|---|---|"]
        for o in rows:
            cs = ", ".join(o.lst("contracts") + [c + " (new)" for c in o.lst("new-contracts")])
            out.append(f"| {link(o)} | {o.meta['title']} | {PROGRAMS.get(o.meta['program'], '?')} "
                       f"| {o.meta['size']} | {status_of(o, by_id)} | {cs or '—'} |")
        out.append("")

    parked = sorted([o for o in orders if o.state == "parked"], key=sort_key)
    if parked:
        out += ["## Parked — on purpose, with what would unpark it", "",
                "| order | title | size | until |", "|---|---|---|---|"]
        for o in parked:
            out.append(f"| {link(o)} | {o.meta['title']} | {o.meta['size']} | {o.meta['parked-until']} |")
        out.append("")

    done = sorted([o for o in orders if o.state == "done"],
                  key=lambda o: (str(o.meta.get("done", "")), o.id), reverse=True)
    if done:
        out += ["## Done", "", "| order | title | done | evidence |", "|---|---|---|---|"]
        for o in done:
            out.append(f"| {link(o)} | {o.meta['title']} | {o.meta['done']} | {o.meta['evidence']} |")
        out.append("")
    return "\n".join(out)


# ── brief ─────────────────────────────────────────────────────────────────────
# Everything here is READ from git and the tree. It never builds, never runs a
# test and never touches the network, so it is safe to run first, every time.

def age(seconds: float) -> str:
    s = max(0, int(seconds))
    for unit, n in (("day", 86400), ("hour", 3600), ("minute", 60)):
        if s >= n:
            v = s // n
            return f"{v} {unit}{'' if v == 1 else 's'} ago"
    return "just now"


def area_of(path: str) -> str:
    """Top-level area of a repo path: two levels under src/ and docs/, one elsewhere."""
    parts = path.strip().strip('"').split("/")
    if len(parts) > 2 and parts[0] in ("src", "docs", "modules"):
        return "/".join(parts[:2])
    return parts[0] if len(parts) > 1 else "(repo root)"


def group_status(porcelain: str) -> dict:
    """`git status --porcelain` -> {area: file count}."""
    out: dict = {}
    for line in porcelain.splitlines():
        if len(line) < 4: continue
        path = line[3:].split(" -> ")[-1]
        out[area_of(path)] = out.get(area_of(path), 0) + 1
    return dict(sorted(out.items(), key=lambda kv: (-kv[1], kv[0])))


def stale_docs(status_md: str) -> tuple[list[str], str]:
    """The stale list from ENGINE_STATUS.md, and the commit it was generated from."""
    m = re.search(r"from commit `([0-9a-f]+)`", status_md)
    items, inside = [], False
    for line in status_md.splitlines():
        if line.startswith("## "):
            inside = "Stale" in line
            continue
        if inside and line.startswith("- "):
            items.append(line[2:].strip())
    return items, (m.group(1) if m else "")


def open_questions(md: str) -> list[dict]:
    """Top-level bullets of open-questions.md: {title, text, where}."""
    items, cur = [], None
    for line in md.splitlines():
        if line.startswith("- **"):
            if cur: items.append(cur)
            m = re.match(r"- \*\*(.+?)\*\*", line)
            cur = {"title": (m.group(1) if m else line[2:]).rstrip("."), "text": line, "where": []}
        elif cur is not None:
            if line.startswith("## ") or line.strip() == "---":
                items.append(cur); cur = None; continue
            cur["text"] += "\n" + line
            w = re.match(r"\s*- \*\*where\*\*\s+(\S+)", line)
            if w: cur["where"].append(w.group(1))
    if cur: items.append(cur)
    return items


def overlapping(questions: list[dict], touches: list[str]) -> list[str]:
    """Open questions that concern a path the order touches: by their `where`
    trailer, or by naming the path (or a file under it) in their text. Most
    questions carry no `where`, so the text match is what makes this useful."""
    def related(a: str, b: str) -> bool:
        a, b = a.rstrip("/"), b.rstrip("/")
        return a == b or a.startswith(b + "/") or b.startswith(a + "/")
    hits = []
    for q in questions:
        named = q["where"] + re.findall(r"`([A-Za-z0-9_./-]+/[A-Za-z0-9_./-]+)`", q["text"])
        if any(related(n, t) for n in named for t in touches):
            hits.append(q["title"])
    return hits


def git(root: Path, *args: str) -> str | None:
    try:
        r = subprocess.run(["git", "-C", str(root), *args], capture_output=True,
                           text=True, timeout=10)
        return r.stdout if r.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def last_test_run(root: Path) -> str:
    """The last LOCAL ctest run, from ctest's own result files — never a run."""
    tmp = root / "build" / "Testing" / "Temporary"
    log = tmp / "LastTest.log"
    if not log.exists():
        return "no local ctest run found (build/Testing/Temporary/LastTest.log)"
    when = age(time.time() - log.stat().st_mtime)
    failed = tmp / "LastTestsFailed.log"
    names = []
    if failed.exists() and failed.stat().st_mtime >= log.stat().st_mtime - 5:
        names = [l.split(":", 1)[-1].strip() for l in failed.read_text().splitlines() if l.strip()]
    return f"last local ctest run {when}: " + describe_run(log.read_text(errors="replace"), names)


def describe_run(log_text: str, failed: list[str]) -> str:
    """How much of the suite a ctest run covered, and its verdict. A run of five
    docs tests must not read like a green suite: 'all passed' alone hid three
    failures from the previous full run the first time this printed."""
    ran = re.findall(r"^(\d+)/(\d+) Testing: ", log_text, re.M)
    total = int(ran[0][1]) if ran else 0
    labels = re.findall(r"^(\w[\w-]*)\s+=\s+[\d.]+ sec\*proc", log_text, re.M)
    scope = f"{len(ran)} of {total} tests" if total else "unknown scope"
    if labels:
        scope += f" (labels: {', '.join(labels)})"
    if total and len(ran) < total:
        scope += " — a PARTIAL run"
    if not failed:
        return f"{scope}, all passed"
    return f"{scope}, {len(failed)} failed — {', '.join(failed)}"


def print_next(orders: list[Order]) -> None:
    by_id = {o.id: o for o in orders}
    act = sorted([o for o in orders if o.state == "active"], key=sort_key)
    if act:
        print("IN PROGRESS")
        for o in act:
            d, t = o.boxes()
            print(f"  {o.id}  {o.meta['title']}  ({d}/{t})   {WORK_DIR}/{o.path.name}")
            for i in o.open_items(): print(f"      [ ] {i}")
        print()
    print("NEXT UP")
    for o in ready(orders, by_id)[:3]:
        print(f"  {o.id}  {o.priority}  {o.meta['size']}  {o.meta['title']}")
        print(f"        {first_line(o)}")
    n_block = sum(1 for o in orders if o.state == "todo" and o not in ready(orders, by_id))
    n_park = sum(1 for o in orders if o.state == "parked")
    print(f"\n{len(orders)} orders · {n_block} blocked · {n_park} parked · board: {WORK_DIR}/{BOARD}")


def brief(root: Path, orders: list[Order], errs: list[str], since: str | None) -> int:
    t0 = time.monotonic()
    print("WHERE YOU ARE")
    head = git(root, "log", "-1", "--format=%h%x09%ct%x09%s")
    if head is None:
        print("  not a git repository — git facts skipped")
    else:
        h, ct, subj = (head.strip().split("\t", 2) + ["", "", ""])[:3]
        print(f"  last commit {h}  {age(time.time() - int(ct or 0))}  {subj}")

        # "Since you last worked": the last commit by YOU before today, unless given.
        anchor, label = since, since
        if not anchor:
            me = (git(root, "config", "user.email") or "").strip() or \
                 (git(root, "config", "user.name") or "").strip()
            today = datetime.now().strftime("%Y-%m-%d 00:00")
            if me:
                anchor = (git(root, "log", "-1", f"--author={me}", f"--before={today}",
                              "--format=%h") or "").strip() or None
                label = f"{anchor} (your last commit before today)" if anchor else None
        if anchor:
            log = git(root, "log", "--format=%h  %s", f"{anchor}..HEAD")
            if log is None:
                print(f"  since {label}: unknown ref")
            else:
                lines = log.strip().splitlines()
                print(f"  since {label}: {len(lines)} commit(s)")
                for l in lines[:12]: print(f"    {l}")
                if len(lines) > 12: print(f"    … {len(lines) - 12} more (git log {anchor}..HEAD)")

        groups = group_status(git(root, "status", "--porcelain") or "")
        if groups:
            n = sum(groups.values())
            print(f"  uncommitted: {n} file(s) — " +
                  ", ".join(f"{a} {c}" for a, c in groups.items()))
        else:
            print("  uncommitted: nothing — the tree is clean")
    print(f"  {last_test_run(root)}")
    print()

    status = root / "ENGINE_STATUS.md"
    if status.exists():
        items, commit = stale_docs(status.read_text(encoding="utf-8"))
        if items:
            print(f"STALE DOCS  (ENGINE_STATUS.md{', generated at ' + commit if commit else ''})")
            for i in items: print(f"  {i}")
            print()

    if errs:
        print(f"WORK ORDERS HAVE {len(errs)} ERROR(S) — fix these first:")
        for x in errs: print("  " + x)
        print()
    else:
        print_next(orders)
        by_id = {o.id: o for o in orders}
        focus = (sorted([o for o in orders if o.state == "active"], key=sort_key)
                 or ready(orders, by_id))
        oq = root / "docs" / "process" / "open-questions.md"
        if focus and oq.exists():
            hits = overlapping(open_questions(oq.read_text(encoding="utf-8")), focus[0].lst("touches"))
            if hits:
                print(f"\nOPEN QUESTIONS TOUCHING {focus[0].id}  (docs/process/open-questions.md)")
                for t in hits: print(f"  {t}")
    print(f"\n(brief: {time.monotonic() - t0:.2f}s, read-only)")
    return 1 if errs else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--root", type=Path, default=REPO, help=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    b = sub.add_parser("board"); b.add_argument("--check", action="store_true")
    sub.add_parser("next")
    br = sub.add_parser("brief", help="regain context: git, lanes, stale docs, the queue")
    br.add_argument("--since", help="a git ref to list commits from (default: your "
                                    "last commit before today)")
    a = ap.parse_args(argv)

    orders, errs = load(a.root)
    errs += check(a.root, orders)
    if a.cmd == "brief":                 # runs even with errors: that is when you need it
        return brief(a.root, orders, errs, a.since)
    if errs:
        print(f"{len(errs)} work-order error(s):")
        for x in errs: print("  " + x)
        return 1

    if a.cmd == "check":
        print(f"{len(orders)} work orders, 0 errors")
        return 0

    if a.cmd == "board":
        text = render(orders)
        path = a.root / WORK_DIR / BOARD
        if a.check:
            cur = path.read_text(encoding="utf-8") if path.exists() else ""
            if cur != text:
                print(f"{WORK_DIR}/{BOARD} is out of date — run: python3 scripts/work_orders.py board")
                return 1
            print(f"{WORK_DIR}/{BOARD} is up to date")
            return 0
        path.write_text(text, encoding="utf-8")
        print(f"wrote {WORK_DIR}/{BOARD} ({len(orders)} orders)")
        return 0

    print_next(orders)
    return 0


if __name__ == "__main__":
    sys.exit(main())
