#!/usr/bin/env python3
"""engine_doctor — the engine's documentation and maturity contract.

Two modes, one parser:

    engine_doctor.py check    validate every doc's front-matter against the
                              tree: paths exist, tests exist, tier claims have
                              mechanical evidence, and as-built docs are not
                              stale (their code changed after `verified:`).

    engine_doctor.py status   regenerate ENGINE_STATUS.md — the single
                              generated answer to "what is actually true right
                              now". `status --check` fails if it is out of
                              date, the way a formatter does.

WHY THIS EXISTS. Documentation that is merely *written* drifts silently: a doc
claimed a 107 ms cache restore that was true only on the day it was measured,
and nothing noticed for a week. The fix is not more discipline, it is making
staleness mechanical — a doc that describes code touched after it was last
verified is a build finding, not a matter of anyone's memory.

Deliberately dependency-free (stdlib only): a checker nobody can run because
of a missing package is a checker that stops being run.
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import date
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
STATUS_FILE = REPO / "ENGINE_STATUS.md"

# ── The contract ─────────────────────────────────────────────────────────────
# status: what KIND of claim the document makes. This drives whether staleness
# even applies — a `target` doc describes something that does not exist yet, so
# "the code changed" is not a defect in it.
STATUSES = {
    "as-built":   "describes code that exists; MUST match the tree",
    "target":     "describes intended design; not yet built",
    "decided":    "records a decision + rationale; frozen unless revisited",
    "reference":  "external/API reference material",
    "plan":       "roadmap / backlog; expected to churn",
    "unreviewed": "bootstrapped, nobody has classified it yet",
}

# tier: maturity of a SUBSYSTEM (info.md only). Each tier's evidence is
# mechanically checkable — that is the entire point. You cannot claim a tier
# you have not earned, so "stable" stops being a matter of opinion.
TIERS = ["prototype", "working", "hardened", "production"]
TIER_RULES = {
    "prototype":  "compiles and runs; no test obligation",
    "working":    "≥1 test listed in `tests:` and those tests exist",
    # "Survives hostile input and time" is measured by the endurance lanes, not
    # by unit tests — so hardened demands evidence from at least one of them,
    # whatever the subsystem's input source. A subsystem that additionally eats
    # untrusted BYTES must specifically be fuzzed.
    "hardened":   "working + ≥1 fuzz/soak/stress test (and specifically a fuzz"
                  " test when parses-external-input) + doc not stale",
    "production": "hardened + exercised on every CI platform + a perf claim"
                  " backed by a test",
}

# Endurance lanes, recognised by test filename (mirrors tests/CMakeLists labels).
ENDURANCE = ("fuzz", "soak", "stress")

REQUIRED = ["status"]                     # every doc
REQUIRED_INFO = ["status", "tier"]        # info.md additionally declares tier

# Docs that are exempt: generated files and third-party trees.
# Build outputs, matched at the REPO ROOT only. Matching them as any path
# component swallowed src/tools/build/info.md — a doc silently invisible to the
# checker, which is the exact failure this tool exists to prevent. Nested
# directories legitimately carry these names (src/tools/build is source).
EXCLUDE_ROOTS = {"build", "build-asan", "build-ship", "dist"}
# These nest by nature: a project's .cache, a kit's build dir, submodule .git
# dirs. Matched at any depth.
# vendored code nests too — modules/assetlib/third_party/ is not ours.
EXCLUDE_DIRS  = {".git", "node_modules", ".cache", ".kitbuild", "third_party"}


def _tracked_docs() -> set[str] | None:
    """Repo-relative *.md paths that git actually TRACKS (None if not a repo).

    The doctor must see the same tree a fresh clone does, or its output is not
    reproducible — and `status --check` compares generated output. Two ways
    that broke:

      * `Kits/` and `fps_shooter/` are gitignored on purpose (they are separate
        repos). Their READMEs exist on a developer machine and nowhere else, so
        a locally generated ENGINE_STATUS.md listed docs CI could not see.
      * A submodule's docs are present or absent depending on whether the
        checkout used `submodules: recursive`. CI's doc lane deliberately does
        not.

    Either way `status --check` failed on every commit no matter what was
    written, which makes the gate noise rather than signal. Tracked-only makes
    the answer identical everywhere by construction.
    """
    try:
        out = subprocess.run(["git", "-C", str(REPO), "ls-files", "*.md"],
                             capture_output=True, text=True, check=False)
    except OSError:
        return None
    if out.returncode != 0:
        return None                      # not a git checkout — count everything
    return {line for line in out.stdout.splitlines() if line}


TRACKED_DOCS = _tracked_docs()


# ── Front-matter parsing ─────────────────────────────────────────────────────
@dataclass
class Doc:
    path: Path
    meta: dict = field(default_factory=dict)
    has_fm: bool = False
    body_start: int = 0

    @property
    def rel(self) -> str:
        return self.path.relative_to(REPO).as_posix()

    @property
    def is_info(self) -> bool:
        return self.path.name == "info.md"

    def get_list(self, key: str) -> list[str]:
        v = self.meta.get(key, [])
        return v if isinstance(v, list) else [v]


def parse_front_matter(path: Path) -> Doc:
    """Minimal YAML subset: `key: value` and `- item` lists between --- fences.

    Intentionally not PyYAML — the schema is six keys deep and a hand parser
    keeps the tool dependency-free (see module docstring).
    """
    doc = Doc(path=path)
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return doc
    if not lines or lines[0].strip() != "---":
        return doc

    end = next((i for i in range(1, len(lines)) if lines[i].strip() == "---"), None)
    if end is None:
        return doc

    doc.has_fm = True
    doc.body_start = end + 1

    # ` # ...` is a trailing comment (YAML rules: whitespace before the hash).
    # Values are paths, which may legitimately contain '#', so only a hash
    # preceded by whitespace ends the value.
    def strip_comment(s: str) -> str:
        return re.sub(r"\s+#.*$", "", s).strip()

    key = None
    for raw in lines[1:end]:
        line = raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue                       # whole-line comment
        if line.lstrip().startswith("- ") and key:
            doc.meta.setdefault(key, [])
            if not isinstance(doc.meta[key], list):
                doc.meta[key] = [doc.meta[key]]
            doc.meta[key].append(strip_comment(line.lstrip()[2:]))
            continue
        m = re.match(r"^([A-Za-z][\w-]*)\s*:\s*(.*)$", line)
        if m:
            key, val = m.group(1), strip_comment(m.group(2))
            doc.meta[key] = val if val else []
    return doc


def submodule_paths() -> set[str]:
    """Repo-relative paths of git submodules.

    Their docs belong to THEIR repo and are governed by its contract — this
    tool must not edit or gate files another project owns (modules/hid is a
    real engine subsystem but a separate deliverable).
    """
    out = set()
    gm = REPO / ".gitmodules"
    if not gm.exists():
        return out
    for line in gm.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"\s*path\s*=\s*(.+)$", line)
        if m:
            out.add(m.group(1).strip())
    return out


def find_docs() -> list[Doc]:
    subs = submodule_paths()
    out = []
    for p in sorted(REPO.rglob("*.md")):
        rel = p.relative_to(REPO).as_posix()
        parts = p.relative_to(REPO).parts
        if parts and parts[0] in EXCLUDE_ROOTS:
            continue
        rel_str = str(p.relative_to(REPO))
        # Untracked/ignored (Kits/, fps_shooter/) and submodule-owned docs are
        # invisible to a fresh clone — see _tracked_docs().
        if TRACKED_DOCS is not None and rel_str not in TRACKED_DOCS:
            continue
        if any(part in EXCLUDE_DIRS for part in parts):
            continue
        if any(rel == s or rel.startswith(s + "/") for s in subs):
            continue                      # another repo's contract
        if p == STATUS_FILE:
            continue                      # generated; not itself a contract
        out.append(parse_front_matter(p))
    return out


# ── Git helpers ──────────────────────────────────────────────────────────────
def git(*args: str) -> str:
    try:
        return subprocess.run(["git", "-C", str(REPO), *args],
                              capture_output=True, text=True,
                              check=False).stdout.strip()
    except OSError:
        return ""


def is_shallow() -> bool:
    """A shallow clone cannot answer 'when did this last change?'.

    Every file's last commit collapses to the tip, so EVERY doc reads as
    changed today and the whole tree looks stale. CI hit exactly this: a
    `fetch-depth: 1` checkout turned the staleness check into a machine that
    fails the gating leg for reasons that do not exist. Detecting it and
    refusing to make the claim beats emitting confident nonsense.
    """
    return git("rev-parse", "--is-shallow-repository") == "true"


def last_change(paths: list[str]) -> str:
    """ISO date of the most recent commit touching any of `paths` ('' if none)."""
    existing = [p for p in paths if (REPO / p.rstrip("/*")).exists()
                or any(REPO.glob(p))]
    if not existing:
        return ""
    out = git("log", "-1", "--format=%cs", "--", *existing)
    return out.splitlines()[0] if out else ""


# Evaluated once: every doc asks the same question, and `git rev-parse` per doc
# would be 60+ subprocesses for one constant.
SHALLOW = is_shallow()


def expand(pattern: str) -> list[Path]:
    """A covers/tests entry -> real paths. Accepts dirs, files and globs."""
    p = REPO / pattern
    if p.exists():
        return [p]
    return [q for q in REPO.glob(pattern) if q.exists()]


# ── Checks ───────────────────────────────────────────────────────────────────
@dataclass
class Finding:
    level: str          # "error" | "warn" | "info"
    doc: str
    msg: str


# ── The bug ledger ───────────────────────────────────────────────────────────
# Every bug that has ever been found, and the test that stops it coming back.
#
# The practice already existed in prose: engineering-standards.md §4.2 says "a
# regression proof for bug fixes — reintroduce the bug, watch the test fail,
# restore." What it lacked was ENFORCEMENT. Fixed bugs were recorded as ✅ marks
# scattered across twelve issues.md files, naming their tests in sentences, so
# deleting a test silently orphaned the proof and nothing anywhere could answer
# "which bugs have no regression test?".
#
# This is the CAD-kernel discipline applied to the whole engine rather than only
# to the parsers: tests/fuzz/corpus/*/regressions.seeds already pins every bug
# the fuzzers found, forever, and it works. The difference is that a seed only
# exists for bugs reachable by feeding bytes to a parser — which is a minority.
# ABI breaks, races, lifetime errors and build-graph mistakes need the same
# permanence and had nowhere to live.
#
# The load-bearing field is `pinned-by`. A ledger entry naming a test that does
# not exist is an ERROR, which is what turns "we should write a test" into a
# gate.
BUG_LEDGER = REPO / "docs" / "process" / "bug-ledger.md"

# The class decides the SHAPE of the pinning test, which is the practical value
# of recording it: it answers "what kind of test do I write?" mechanically
# instead of leaving it to judgement each time.
BUG_CLASSES = {
    "memory":    "out-of-bounds, use-after-free, leak     -> ASan/UBSan lane + a targeted test",
    "threading": "race, deadlock, livelock                -> TSan lane + stress",
    "abi":       "layout, versioning, symbol contract     -> static asserts + an OFFSET test",
    "parse":     "malformed or hostile input              -> a corpus seed",
    "numeric":   "determinism, precision, overflow        -> golden-value test",
    "perf":      "a regression in time or memory          -> perf test with a threshold",
    "build":     "link graph, target wiring, packaging    -> assert on the built artifact",
    "logic":     "plain wrong behaviour                   -> ordinary unit test",
    "coverage":  "a check that silently never ran         -> make it run, then assert it",
}

BUG_REQUIRED = ("found", "class", "where", "symptom", "cause", "pinned-by")

_BUG_HEAD = re.compile(r"^##\s+(BUG-\d{4})\b\s*(?:—|-)?\s*(.*)$")
_BUG_FIELD = re.compile(r"^-\s+([a-z-]+):\s*(.*)$")


def parse_bug_ledger() -> list[dict]:
    """Entries as dicts. Format is deliberately stdlib-parseable — this script
    has no third-party dependencies and adding one for a bug list would be a
    poor trade."""
    if not BUG_LEDGER.exists():
        return []
    entries: list[dict] = []
    cur: dict | None = None
    for lineno, raw in enumerate(BUG_LEDGER.read_text(encoding="utf-8").splitlines(), 1):
        head = _BUG_HEAD.match(raw)
        if head:
            cur = {"id": head.group(1), "title": head.group(2).strip(), "line": lineno}
            entries.append(cur)
            continue
        if cur is None:
            continue
        fld = _BUG_FIELD.match(raw)
        if fld:
            cur[fld.group(1)] = fld.group(2).strip()
    return entries


def check_bugs() -> list[Finding]:
    f: list[Finding] = []
    rel = str(BUG_LEDGER.relative_to(REPO))
    entries = parse_bug_ledger()
    if not entries:
        return [Finding("warn", rel, "no ledger entries found")]

    seen: dict[str, int] = {}
    for e in entries:
        bid = e["id"]
        if bid in seen:
            f.append(Finding("error", rel,
                             f"{bid} declared twice (lines {seen[bid]} and {e['line']}) "
                             f"— ids are permanent, never reused"))
        seen[bid] = e["line"]

        for key in BUG_REQUIRED:
            if not e.get(key):
                f.append(Finding("error", rel, f"{bid}: missing `{key}:`"))

        cls = e.get("class", "")
        if cls and cls not in BUG_CLASSES:
            f.append(Finding("error", rel,
                             f"{bid}: unknown class `{cls}` "
                             f"(want: {'/'.join(sorted(BUG_CLASSES))})"))

        # THE GATE. A regression proof that does not exist is not a proof, and a
        # ledger of unenforced good intentions is worse than no ledger — it
        # reads as coverage that is not there.
        for field in ("pinned-by", "where"):
            spec = e.get(field, "")
            if not spec or spec.lower() in ("none", "n/a"):
                continue
            for item in (p.strip() for p in spec.split(",")):
                path = item.split("::", 1)[0].strip()   # allow file::case
                if not path:
                    continue
                if not (REPO / path).exists():
                    lvl = "error" if field == "pinned-by" else "warn"
                    f.append(Finding(lvl, rel,
                                     f"{bid}: `{field}: {path}` does not exist"
                                     + (" — the regression proof is orphaned"
                                        if field == "pinned-by" else "")))
    return f


def check_docs(docs: list[Doc], strict_missing: bool) -> list[Finding]:
    f: list[Finding] = []
    today = date.today().isoformat()

    for d in docs:
        if not d.has_fm:
            # Missing front-matter is a WARNING by default so the contract can
            # be adopted incrementally instead of blocking on a 29-file rewrite.
            f.append(Finding("error" if strict_missing else "warn", d.rel,
                             "no front-matter (run: engine_doctor.py bootstrap)"))
            continue

        for key in (REQUIRED_INFO if d.is_info else REQUIRED):
            if key not in d.meta or d.meta[key] == []:
                f.append(Finding("error", d.rel, f"missing required key `{key}:`"))

        status = d.meta.get("status", "")
        if status and status not in STATUSES:
            f.append(Finding("error", d.rel,
                             f"unknown status `{status}` (want: {'/'.join(STATUSES)})"))

        tier = d.meta.get("tier", "")
        if tier and tier not in TIERS:
            f.append(Finding("error", d.rel,
                             f"unknown tier `{tier}` (want: {'/'.join(TIERS)})"))

        # Referenced paths must exist. A doc pointing at a file that was moved
        # or deleted is the cheapest possible drift signal, so it is an error.
        for key in ("covers", "tests"):
            for entry in d.get_list(key):
                if not expand(entry):
                    f.append(Finding("error", d.rel,
                                     f"`{key}:` path does not exist: {entry}"))

        verified = str(d.meta.get("verified", "") or "")
        if verified and not re.match(r"^\d{4}-\d{2}-\d{2}$", verified):
            f.append(Finding("error", d.rel,
                             f"`verified:` must be YYYY-MM-DD, got `{verified}`"))
        elif verified > today:
            f.append(Finding("error", d.rel,
                             f"`verified:` is in the future ({verified})"))

        # ── Staleness: the whole reason this tool exists ──────────────────
        # Only as-built docs make claims about current code, so only they can
        # go stale. Compare the doc's verified date against the last commit
        # touching the code it covers.
        if status == "as-built":
            covers = d.get_list("covers")
            if not covers:
                f.append(Finding("error", d.rel,
                                 "status `as-built` requires `covers:` "
                                 "(otherwise staleness cannot be checked)"))
            elif not verified:
                f.append(Finding("error", d.rel,
                                 "status `as-built` requires `verified:`"))
            else:
                # Shallow clones cannot answer this — see is_shallow().
                changed = "" if SHALLOW else last_change(covers)
                if changed and changed > verified:
                    f.append(Finding("warn", d.rel,
                                     f"STALE: covered code changed {changed}, "
                                     f"doc verified {verified} — re-verify and "
                                     f"bump `verified:`"))

        # ── Tier evidence ────────────────────────────────────────────────
        if d.is_info and tier:
            tests = d.get_list("tests")
            if tier in ("working", "hardened", "production") and not tests:
                f.append(Finding("error", d.rel,
                                 f"tier `{tier}` requires ≥1 entry in `tests:` "
                                 f"— {TIER_RULES[tier]}"))
            if tier in ("hardened", "production"):
                endurance = [t for t in tests
                             if any(k in Path(t).name.lower() for k in ENDURANCE)]
                if not endurance:
                    f.append(Finding("error", d.rel,
                                     f"tier `{tier}` requires ≥1 fuzz/soak/stress "
                                     "test in `tests:` — unit tests alone do not "
                                     "show it survives hostile input or time"))
                if d.meta.get("parses-external-input", "").lower() in ("1", "true", "yes"):
                    fuzzy = [t for t in tests if "fuzz" in Path(t).name.lower()]
                    if not fuzzy:
                        f.append(Finding("error", d.rel,
                                         f"tier `{tier}` + parses-external-input "
                                         "requires a FUZZ test in `tests:` (untrusted "
                                         "bytes need a fuzzer, not just stress)"))
                if status == "as-built" and verified and not SHALLOW:
                    changed = last_change(d.get_list("covers"))
                    if changed and changed > verified:
                        f.append(Finding("error", d.rel,
                                         f"tier `{tier}` requires a fresh doc, "
                                         f"but it is stale (code {changed} > "
                                         f"verified {verified})"))
            if tier == "production" and not d.meta.get("perf-test"):
                f.append(Finding("warn", d.rel,
                                 "tier `production` wants `perf-test:` naming the "
                                 "test that holds its performance claim"))
    return f


# ── Status generation ────────────────────────────────────────────────────────
def subsystem_rows(docs: list[Doc]) -> list[dict]:
    rows = []
    for d in docs:
        if not d.is_info:
            continue
        covers = d.get_list("covers") or [str(d.path.parent.relative_to(REPO))]
        tests = d.get_list("tests")
        changed = last_change(covers)
        verified = str(d.meta.get("verified", "") or "")
        stale = bool(changed and verified and changed > verified)
        rows.append({
            # An info.md does not always sit at the root of what it describes
            # (src/runtime's lives in src/runtime/docs/), so the first `covers:`
            # entry names the subsystem when present.
            "name": (covers[0].rstrip("/*") if d.meta.get("covers")
                     else d.path.parent.relative_to(REPO).as_posix()),
            "tier": d.meta.get("tier", "—"),
            "status": d.meta.get("status", "—"),
            "verified": verified or "never",
            "stale": stale,
            "tests": tests,
            "changed": changed or "—",
        })
    rows.sort(key=lambda r: (TIERS.index(r["tier"]) if r["tier"] in TIERS else -1,
                             r["name"]), reverse=True)
    return rows


def render_status(docs: list[Doc]) -> str:
    rows = subsystem_rows(docs)
    findings = check_docs(docs, strict_missing=False)
    errs = [x for x in findings if x.level == "error"]
    stale = [r for r in rows if r["stale"]]
    unreviewed = [d for d in docs if not d.has_fm
                  or d.meta.get("status") == "unreviewed"]

    out = []
    out.append("# Engine Status")
    out.append("")
    out.append("<!-- GENERATED by scripts/engine_doctor.py — do not edit by hand.")
    out.append("     Regenerate: python3 scripts/engine_doctor.py status -->")
    out.append("")
    out.append(f"Generated {date.today().isoformat()} from "
               f"commit `{git('rev-parse', '--short', 'HEAD') or '?'}`.")
    out.append("")
    out.append("The one place that answers *what is actually true right now*. "
               "Every column is derived from the tree — tiers come from each "
               "subsystem's `info.md`, freshness from git. Nothing here is "
               "hand-maintained, so nothing here can quietly go out of date.")
    out.append("")

    # Headline counts
    by_tier = {t: sum(1 for r in rows if r["tier"] == t) for t in TIERS}
    out.append("## Summary")
    out.append("")
    out.append(f"- **Subsystems tracked:** {len(rows)}")
    for t in reversed(TIERS):
        out.append(f"- **{t}:** {by_tier[t]}")
    out.append(f"- **Stale docs:** {len(stale)}")
    out.append(f"- **Contract errors:** {len(errs)}")
    out.append(f"- **Unreviewed docs:** {len(unreviewed)}")
    out.append("")

    out.append("## Subsystems")
    out.append("")
    out.append("| subsystem | tier | doc | verified | code last changed | tests |")
    out.append("|---|---|---|---|---|---|")
    for r in rows:
        flag = " ⚠️" if r["stale"] else ""
        tests = ", ".join(f"`{Path(t).name}`" for t in r["tests"]) or "—"
        out.append(f"| `{r['name']}` | {r['tier']} | {r['status']}{flag} | "
                   f"{r['verified']} | {r['changed']} | {tests} |")
    out.append("")

    if stale:
        out.append("## ⚠️ Stale — code moved after the doc was last verified")
        out.append("")
        for r in stale:
            out.append(f"- `{r['name']}` — code {r['changed']}, "
                       f"verified {r['verified']}")
        out.append("")

    if errs:
        out.append("## Contract errors")
        out.append("")
        for e in errs:
            out.append(f"- `{e.doc}` — {e.msg}")
        out.append("")

    if unreviewed:
        out.append("## Unreviewed docs")
        out.append("")
        out.append("Bootstrapped or unclassified — they make no checkable claim yet.")
        out.append("")
        for d in unreviewed:
            out.append(f"- `{d.rel}`")
        out.append("")

    out.append("## Tier ladder")
    out.append("")
    out.append("See `docs/process/engineering-standards.md`. Each tier's evidence is "
               "checked mechanically by `engine_doctor.py check`:")
    out.append("")
    for t in TIERS:
        out.append(f"- **{t}** — {TIER_RULES[t]}")
    out.append("")
    return "\n".join(out) + "\n"


# ── Bootstrap ────────────────────────────────────────────────────────────────
def bootstrap(docs: list[Doc]) -> int:
    """Insert honest placeholder front-matter into docs that have none.

    Deliberately writes `status: unreviewed` / no `verified:` rather than
    guessing: fabricated metadata is worse than none, because it *looks*
    checked. These show up in ENGINE_STATUS.md as unreviewed until a human
    classifies them.
    """
    n = 0
    for d in docs:
        if d.has_fm:
            continue
        fm = ["---", "status: unreviewed"]
        if d.is_info:
            fm.append("tier: prototype")
            fm.append(f"covers:\n  - {d.path.parent.relative_to(REPO).as_posix()}/")
        fm += ["---", ""]
        text = d.path.read_text(encoding="utf-8", errors="replace")
        d.path.write_text("\n".join(fm) + text, encoding="utf-8")
        print(f"  bootstrapped {d.rel}")
        n += 1
    print(f"bootstrap: {n} file(s) updated")
    return 0


# ── Entry ────────────────────────────────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    c = sub.add_parser("check", help="validate the doc contract")
    c.add_parser = None  # noqa
    c.add_argument("--strict-missing", action="store_true",
                   help="treat missing front-matter as an error (post-adoption)")
    c.add_argument("--warnings-as-errors", action="store_true")

    s = sub.add_parser("status", help="regenerate ENGINE_STATUS.md")
    s.add_argument("--check", action="store_true",
                   help="fail if ENGINE_STATUS.md is out of date (CI mode)")

    sub.add_parser("bootstrap", help="add placeholder front-matter where missing")

    b = sub.add_parser("bugs", help="summarise the bug ledger")
    b.add_argument("--class", dest="klass", help="filter by class")

    args = ap.parse_args()
    docs = find_docs()

    if args.cmd == "bootstrap":
        return bootstrap(docs)

    if args.cmd == "bugs":
        entries = parse_bug_ledger()
        if args.klass:
            entries = [e for e in entries if e.get("class") == args.klass]
        by_class: dict[str, int] = {}
        for e in entries:
            by_class[e.get("class", "?")] = by_class.get(e.get("class", "?"), 0) + 1
            print(f"{e['id']}  {e.get('class','?'):<10} {e.get('title','')}")
            print(f"          pinned-by: {e.get('pinned-by','(none)')}")
        print(f"\n{len(entries)} bug(s)")
        for k in sorted(by_class):
            print(f"  {k:<10} {by_class[k]}")
        problems = check_bugs()
        for f in problems:
            print(f"{f.level.upper():<5}  {f.msg}")
        return 1 if any(f.level == "error" for f in problems) else 0

    if args.cmd == "status":
        text = render_status(docs)
        if args.check:
            current = STATUS_FILE.read_text(encoding="utf-8") if STATUS_FILE.exists() else ""

            # ── Why this comparison ignores every date ───────────────────────
            # A GENERATED FILE CANNOT BE GATED ON CONTENT THAT CHANGES BECAUSE OF
            # THE COMMIT THAT CONTAINS IT. Parts of this document are derived
            # from git history — the per-subsystem "code last changed" column and
            # the stale-doc count — so regenerating BEFORE a commit and
            # regenerating AFTER it give different answers by construction: the
            # working-tree edits are not in git history yet, and the moment they
            # are, every doc covering them moves. Committing the file therefore
            # invalidates it, and `--check` failed on the gating leg for three
            # runs in a row while the file was, in every sense anyone cared
            # about, up to date. (Stale docs read 7 before the commit and 14
            # after — same tree, same script.)
            #
            # Two clauses above this function already record the same lesson
            # from two other causes: a gate that cannot be satisfied is noise,
            # not signal, and people learn to ignore the whole lane.
            #
            # So the gate keeps what it can actually enforce — the STRUCTURE: the
            # set of subsystems, their tiers, their docs, their test counts. Add
            # a subsystem, change a tier, or delete an info.md without
            # regenerating and this still fails. Dates are normalised away, and
            # freshness stays the job of `engine_doctor check`, which computes it
            # live and reports it as warnings rather than freezing it into a file.
            DATE_RE = re.compile(r"\d{4}-\d{2}-\d{2}")

            def stable(t: str) -> str:
                keep = []
                for l in t.splitlines():
                    if l.startswith("Generated "):
                        continue          # changes daily, and names the commit
                    if l.startswith("- **Stale docs:**"):
                        continue          # a function of git history, not tree
                    keep.append(DATE_RE.sub("<date>", l))
                return "\n".join(keep)

            if stable(current) != stable(text):
                print("ENGINE_STATUS.md is out of date — run: "
                      "python3 scripts/engine_doctor.py status", file=sys.stderr)
                return 1
            print("ENGINE_STATUS.md is up to date")
            return 0
        STATUS_FILE.write_text(text, encoding="utf-8")
        print(f"wrote {STATUS_FILE.relative_to(REPO)}")
        return 0

    findings = check_docs(docs, strict_missing=args.strict_missing)
    findings += check_bugs()
    errs = [f for f in findings if f.level == "error"]
    warns = [f for f in findings if f.level == "warn"]

    for f in sorted(findings, key=lambda x: (x.level != "error", x.doc)):
        print(f"{'ERROR' if f.level == 'error' else 'warn ':<5}  {f.doc}: {f.msg}")

    print(f"\nengine_doctor: {len(docs)} doc(s), "
          f"{len(errs)} error(s), {len(warns)} warning(s)")
    if errs or (args.warnings_as_errors and warns):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
