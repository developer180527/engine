#!/usr/bin/env python3
"""A runtime binary carries none of the cook stack (WO-017).

engine_player, the server and every game load COOKED content. The cook stack
(Assimp, the import front ends, the cookers, the texture encoders) is the
engine_cooking library, and only dev tools link it. This reads a linked
binary's symbol table and fails if any of it got in anyway.

CI already checked this for Assimp in the SHIPPING build, where the cook stack
is not compiled at all. That proves the build setting, not the boundary: the
default (dev) build compiles the whole stack, and until WO-017 engine_player
linked all of it there. This runs on whatever build ctest runs in, the default
one included, and names what it finds.

Run: python3 scripts/check_no_cook_stack.py <binary> [<binary> ...]
"""
from __future__ import annotations

import re
import shutil
import subprocess
import sys
from pathlib import Path

# Substrings of (mangled or plain) symbol names that only the cook stack
# defines. Case-insensitive. Each is a namespace or class of engine_cooking,
# or a library only it links.
MARKERS = {
    "assimp":        "Assimp (the source-format parser)",
    "meshcook":      "the mesh cooker",
    "clipcook":      "the clip cooker",
    "shadercook":    "the shader cooker",
    "matcook":       "the material cooker",
    "MeshCooker":    "the mesh cooker",
    "TextureCooker": "the texture cooker",
    "CookService":   "the cook service",
    "rgbcx":         "the BC1-5 encoder",
    "bc7enc":        "the BC7 encoder",
    "imageEncode":   "bimg's encoders (ASTC/ETC2)",
    # Since WO-018 the runtime parses no source format at all, glTF and
    # source images included: these are the cook stack's decoders now.
    "cgltf_":        "cgltf (the glTF parser)",
    "stbi_":         "stb_image (source image decoding)",
}
_PAT = re.compile("|".join(re.escape(m) for m in MARKERS), re.I)


def symbol_lines(binary: str) -> tuple[list[str] | None, str]:
    """The binary's linked symbols, one per line, and where they came from.

    MSVC keeps symbols in the PDB, not the .exe, so `nm` on a Windows binary
    either reads nothing (x64: an empty table, which used to PASS) or cannot
    read it at all (MinGW nm on ARM64 PE: "file format not recognized"). There
    the linker's map is the record of what was linked: tests/CMakeLists.txt asks
    MSVC for one (/MAP) beside each binary this checks (WO-038).
    """
    mapfile = Path(binary).with_suffix(".map")
    if mapfile.exists():
        return mapfile.read_text(encoding="utf-8", errors="ignore").splitlines(), f"the link map {mapfile.name}"
    nm = shutil.which("nm")
    if not nm:
        return None, "no `nm` on this host and no link map beside the binary"
    p = subprocess.run([nm, binary], capture_output=True, text=True, check=False)
    if p.returncode != 0:
        return None, f"nm failed: {p.stderr.strip()}"
    return p.stdout.splitlines(), "nm"


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    failed = False
    for binary in sys.argv[1:]:
        lines, source = symbol_lines(binary)
        # Nothing to read is not a pass: an empty table proves nothing about
        # what was linked, which is how Windows x64 passed with nothing checked.
        if not lines:
            print(f"FAIL  {binary}: no symbols to check ({source})")
            failed = True
            continue
        hits: dict[str, list[str]] = {}
        for line in lines:
            # MSVC's map names a string literal by its text (`??_C@_0L@...@ShaderCook?$AA@`),
            # so a log channel called "ShaderCook" read as the shader cooker.
            # nm never lists literals; neither does this.
            if "??_C@" in line:
                continue
            m = _PAT.search(line)
            if m:
                key = next(k for k in MARKERS if k.lower() == m.group(0).lower())
                hits.setdefault(key, []).append(line.split()[-1])
        if hits:
            failed = True
            print(f"FAIL  {binary} carries the cook stack:")
            for key, syms in sorted(hits.items()):
                print(f"        {MARKERS[key]}: {len(syms)} symbol(s), e.g. {syms[0]}")
        else:
            print(f"ok    {binary}: no cook-stack symbols ({len(lines)} lines from {source})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
