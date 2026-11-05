#!/usr/bin/env python3
"""The shared C API library exports exactly the header's functions (WO-070).

Two directions, both failures:
  - a symbol exported that is not an eng* function: engine internals leaked,
    which a binding could then depend on;
  - a function declared in include/eng/*.h that is not exported: the header
    promises something the library does not have.

And a third check, given the padding check's source: every struct the headers
declare is laid out there (Clang reports padding only in structs a translation
unit uses, so a struct missing from that list would escape -Wpadded).

Run: python3 scripts/check_eng_exports.py <libeng.so|libeng.dylib> <include/eng> [tests/eng_header_check.c]
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


def exported(lib: str) -> set[str]:
    for cmd in (["nm", "-gU", lib], ["nm", "-D", "--defined-only", lib]):
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode == 0 and p.stdout.strip():
            out = set()
            for line in p.stdout.splitlines():
                parts = line.split()
                if len(parts) < 3 or parts[1] in ("A",):   # A: a version-script node, a label
                    continue
                out.add(parts[2].lstrip("_") if sys.platform == "darwin" else parts[2])
            return out
    sys.exit(f"FAIL: could not read the symbols of {lib}")


def declared(include: str) -> set[str]:
    names = set()
    for h in sorted(Path(include).glob("*.h")):
        text = re.sub(r"/\*.*?\*/", "", h.read_text(), flags=re.S)
        names.update(re.findall(r"ENG_API\s+[\w\s\*]+?\b(eng[A-Z]\w*)\s*\(", text))
    return names


def main() -> int:
    lib, include = sys.argv[1], sys.argv[2]
    have, want = exported(lib), declared(include)
    extra = sorted(s for s in have if not re.fullmatch(r"eng[A-Z]\w*", s))
    missing = sorted(want - have)
    unexpected = sorted(s for s in have if re.fullmatch(r"eng[A-Z]\w*", s) and s not in want)
    bad = 0
    if extra:
        print(f"FAIL: {len(extra)} non-eng symbol(s) exported, e.g. {extra[:5]}"); bad = 1
    if missing:
        print(f"FAIL: declared in include/eng but not exported: {missing}"); bad = 1
    if unexpected:
        print(f"FAIL: exported eng* functions no header declares: {unexpected}"); bad = 1
    if not bad:
        print(f"ok    {len(want)} functions declared, the same {len(have)} exported, nothing else")
    if len(sys.argv) > 3:
        structs = set()
        for h in sorted(Path(include).glob("*.h")):
            structs.update(re.findall(r"typedef struct (Eng\w+) \{", h.read_text()))
        checked = set(re.findall(r"sizeof\((Eng\w+)\)", Path(sys.argv[3]).read_text()))
        unlisted = sorted(structs - checked)
        if unlisted:
            print(f"FAIL: structs not laid out by the padding check: {unlisted}"); bad = 1
        else:
            print(f"ok    all {len(structs)} structs are laid out by the padding check")
    return bad


if __name__ == "__main__":
    sys.exit(main())
