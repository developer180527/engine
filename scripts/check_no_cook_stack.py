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


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    nm = shutil.which("nm")
    if not nm:
        # MSVC hosts have dumpbin, not nm. Say so rather than pass silently.
        print("check_no_cook_stack: SKIPPED — no `nm` on this host")
        return 0
    failed = False
    for binary in sys.argv[1:]:
        p = subprocess.run([nm, binary], capture_output=True, text=True, check=False)
        if p.returncode != 0:
            print(f"check_no_cook_stack: nm failed on {binary}: {p.stderr.strip()}")
            return 1
        hits: dict[str, list[str]] = {}
        for line in p.stdout.splitlines():
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
            print(f"ok    {binary}: no cook-stack symbols")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
