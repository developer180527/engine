#!/usr/bin/env python3
"""Every edit to a kit-ABI component header is recorded, as a meaning change or not (WO-051).

The loader refuses a kit whose shared components changed LAYOUT, and since
WO-051 one whose components changed MEANING (each type's kAbiRevision). The
layout is measured; the meaning is not. Nothing stops an edit that changes what
a component means from leaving kAbiRevision alone, which is how WO-048 changed
CollisionEvents and every old kit still loaded.

So every such header is recorded in tests/fixtures/kit_abi_headers.txt: a hash
of its text, a hash of its code with comments and blank space removed, and its
revision. Any edit fails --check until it is recorded as one of:

  comment-only     accepted only if the code hash is unchanged
  revision-bumped  accepted only if kAbiRevision went UP

There is no third way: a code change that keeps the revision cannot be recorded.
If it truly keeps the meaning (a new helper method, say), that is still a bump
of the meaning a kit compiled against, and costs one rebuild.

Run:
  python3 scripts/kit_abi_headers.py --check
  python3 scripts/kit_abi_headers.py --record <header> --as comment-only|revision-bumped
  python3 scripts/kit_abi_headers.py --init     (writes the record from scratch)
"""
from __future__ import annotations

import argparse
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests" / "fixtures" / "kit_abi_headers.txt"
MODULE_H = ROOT / "include" / "engine" / "game_module.h"

# ENGINE_ABI_COMPONENTS is the list; the header each type lives in is found by
# its definition, so a component added to the list is covered without editing
# this script.
SEARCH = [ROOT / "src"]


def components() -> list[str]:
    text = MODULE_H.read_text(encoding="utf-8")
    m = re.search(r"#define ENGINE_ABI_COMPONENTS\(X\)((?:.*\\\n)*.*\n)", text)
    if not m:
        sys.exit(f"{MODULE_H}: no ENGINE_ABI_COMPONENTS list")
    return re.findall(r"X\((\w+)\)", m.group(1))


def header_of(type_name: str) -> Path:
    pat = re.compile(rf"^struct {type_name}\s*\{{", re.M)
    hits = [p for d in SEARCH for p in d.rglob("*.h") if pat.search(p.read_text(encoding="utf-8", errors="ignore"))]
    if len(hits) != 1:
        sys.exit(f"{type_name}: expected one header defining it, found {[str(h) for h in hits]}")
    return hits[0]


def strip_comments(text: str) -> str:
    # Strings are left alone; no kit-ABI header has a // or /* inside one.
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"\s+", " ", text).strip()


def revision(type_name: str, text: str) -> int:
    body = re.search(rf"^struct {type_name}\s*\{{(.*?)^\}};", text, re.S | re.M)
    m = body and re.search(r"static\s+constexpr\s+uint32_t\s+kAbiRevision\s*=\s*(\d+)", body.group(1))
    if not m:
        sys.exit(f"{type_name}: no `static constexpr uint32_t kAbiRevision = N;` in its struct")
    return int(m.group(1))


def h(s: str) -> str:
    return hashlib.sha256(s.encode("utf-8")).hexdigest()[:16]


def measure() -> dict[str, tuple[str, str, int, str]]:
    """type -> (full hash, code hash, revision, header path)."""
    out = {}
    for t in components():
        p = header_of(t)
        text = p.read_text(encoding="utf-8").replace("\r\n", "\n")
        out[t] = (h(text), h(strip_comments(text)), revision(t, text), p.relative_to(ROOT).as_posix())
    return out


def read_golden() -> dict[str, tuple[str, str, int, str]]:
    rows = {}
    if GOLDEN.exists():
        for line in GOLDEN.read_text(encoding="utf-8").splitlines():
            if not line or line.startswith("#"):
                continue
            t, full, code, rev, path = line.split()
            rows[t] = (full, code, int(rev), path)
    return rows


def write_golden(rows: dict[str, tuple[str, str, int, str]]) -> None:
    lines = ["# Kit-ABI component headers, recorded (WO-051). Written by scripts/kit_abi_headers.py;",
             "# do not edit by hand. type  text-hash  code-hash  kAbiRevision  header"]
    lines += [f"{t} {f} {c} {r} {p}" for t, (f, c, r, p) in sorted(rows.items())]
    GOLDEN.write_text("\n".join(lines) + "\n", encoding="utf-8")


def check() -> int:
    now, was = measure(), read_golden()
    bad = 0
    for t, (full, code, rev, path) in sorted(now.items()):
        old = was.get(t)
        if old is None:
            print(f"FAIL  {t} ({path}) is in the kit ABI and not recorded: "
                  f"--record {path} --as revision-bumped")
            bad += 1
        elif old[0] != full:
            how = "comment-only" if old[1] == code else (
                  "revision-bumped" if rev > old[2] else None)
            if how:
                print(f"FAIL  {path} changed and is not recorded. If that is right: "
                      f"python3 scripts/kit_abi_headers.py --record {path} --as {how}")
            else:
                print(f"FAIL  {path}: {t}'s code changed and kAbiRevision is still {rev}. "
                      f"A kit built before this edit would load and misread it: bump "
                      f"kAbiRevision, add a note to docs/guides/kit-abi-revisions.md, then record it")
            bad += 1
        else:
            print(f"ok    {t} r{rev} ({path})")
    for t in sorted(set(was) - set(now)):
        print(f"FAIL  {t} is recorded and no longer in ENGINE_ABI_COMPONENTS (re-run --init if intended)")
        bad += 1
    return 1 if bad else 0


def record(header: str, how: str) -> int:
    now, was = measure(), read_golden()
    target = Path(header).resolve().relative_to(ROOT).as_posix() if Path(header).exists() else header
    types = [t for t, v in now.items() if v[3] == target]
    if not types:
        print(f"{header}: defines no kit-ABI component")
        return 2
    for t in types:
        full, code, rev, path = now[t]
        old = was.get(t)
        if old and how == "comment-only" and old[1] != code:
            print(f"refused: {path}'s code changed, not only its comments")
            return 1
        if old and how == "revision-bumped" and rev <= old[2]:
            print(f"refused: {t}.kAbiRevision is {rev}, recorded {old[2]}: it did not go up")
            return 1
        was[t] = now[t]
        print(f"recorded {t} r{rev} ({how})")
    write_golden(was)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--check", action="store_true")
    g.add_argument("--record", metavar="HEADER")
    g.add_argument("--init", action="store_true")
    ap.add_argument("--as", dest="how", choices=["comment-only", "revision-bumped"])
    a = ap.parse_args()
    if a.check:
        return check()
    if a.init:
        write_golden(measure())
        print(f"wrote {GOLDEN.relative_to(ROOT)}")
        return 0
    if not a.how:
        ap.error("--record needs --as comment-only|revision-bumped")
    return record(a.record, a.how)


if __name__ == "__main__":
    sys.exit(main())
