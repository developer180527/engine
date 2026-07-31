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
    "hardened":   "working + fuzz/soak coverage if it parses external input"
                  " + doc not stale",
    "production": "hardened + exercised on every CI platform + a perf claim"
                  " backed by a test",
}

REQUIRED = ["status"]                     # every doc
REQUIRED_INFO = ["status", "tier"]        # info.md additionally declares tier

# Docs that are exempt: generated files and third-party trees.
EXCLUDE_DIRS = {"build", "build-asan", "build-ship", "third_party", ".git",
                "node_modules", "dist", ".cache", ".kitbuild"}


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
    key = None
    for raw in lines[1:end]:
        line = raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if line.lstrip().startswith("- ") and key:
            doc.meta.setdefault(key, [])
            if not isinstance(doc.meta[key], list):
                doc.meta[key] = [doc.meta[key]]
            doc.meta[key].append(line.lstrip()[2:].strip())
            continue
        m = re.match(r"^([A-Za-z][\w-]*)\s*:\s*(.*)$", line)
        if m:
            key, val = m.group(1), m.group(2).strip()
            doc.meta[key] = val if val else []
    return doc


def find_docs() -> list[Doc]:
    out = []
    for p in sorted(REPO.rglob("*.md")):
        if any(part in EXCLUDE_DIRS for part in p.relative_to(REPO).parts):
            continue
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


def last_change(paths: list[str]) -> str:
    """ISO date of the most recent commit touching any of `paths` ('' if none)."""
    existing = [p for p in paths if (REPO / p.rstrip("/*")).exists()
                or any(REPO.glob(p))]
    if not existing:
        return ""
    out = git("log", "-1", "--format=%cs", "--", *existing)
    return out.splitlines()[0] if out else ""


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
                changed = last_change(covers)
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
                if d.meta.get("parses-external-input", "").lower() in ("1", "true", "yes"):
                    fuzzy = [t for t in tests
                             if "fuzz" in t.lower() or "soak" in t.lower()
                             or "stress" in t.lower()]
                    if not fuzzy:
                        f.append(Finding("error", d.rel,
                                         f"tier `{tier}` + parses-external-input "
                                         "requires a fuzz/soak/stress test in `tests:`"))
                if status == "as-built" and verified:
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
            "name": d.path.parent.relative_to(REPO).as_posix(),
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
    out.append("See `docs/engineering-standards.md`. Each tier's evidence is "
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

    args = ap.parse_args()
    docs = find_docs()

    if args.cmd == "bootstrap":
        return bootstrap(docs)

    if args.cmd == "status":
        text = render_status(docs)
        if args.check:
            current = STATUS_FILE.read_text(encoding="utf-8") if STATUS_FILE.exists() else ""
            # The generated-on line changes daily; compare everything else.
            def strip_date(t: str) -> str:
                return "\n".join(l for l in t.splitlines()
                                 if not l.startswith("Generated "))
            if strip_date(current) != strip_date(text):
                print("ENGINE_STATUS.md is out of date — run: "
                      "python3 scripts/engine_doctor.py status", file=sys.stderr)
                return 1
            print("ENGINE_STATUS.md is up to date")
            return 0
        STATUS_FILE.write_text(text, encoding="utf-8")
        print(f"wrote {STATUS_FILE.relative_to(REPO)}")
        return 0

    findings = check_docs(docs, strict_missing=args.strict_missing)
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
