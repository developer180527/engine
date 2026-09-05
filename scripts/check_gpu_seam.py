#!/usr/bin/env python3
"""G1's guard: nothing outside the renderer may name a graphics API.

The engine used to parse assets and upload them in the same function, in five
files spread across src/assets/ and src/runtime/. That coupling is what made a
headless dedicated server, a second backend and an embedding host all
impossible at once, and removing it is docs/rhi/phases.md G1.

Removing it is the easy half. KEEPING it removed is this script, because the
coupling does not come back as a decision — it comes back as one `#include
<bgfx/bgfx.h>` added to fix one compile error, in a file nobody thought of as
graphics code.

Run: python3 scripts/check_gpu_seam.py
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The renderer IS allowed a backend — the point of G1 is that the backend lives
# in one place, not that it lives nowhere. Everything under src/render/ may
# include bgfx, and src/render/gpu.cpp is the door the asset path goes through.
ALLOWED_PREFIXES = (
    "src/render/",
    # The editor's ImGui backend is a RENDERING backend that happens to live
    # next to the editor; it draws with bgfx by nature. It is not the asset
    # path, and porting it is the editor rewrite's problem, not G1's.
    "src/editor/imgui/",
    "src/editor/panels/game_view_panel.h",
    # Test fixtures that stand up a Noop device on purpose. They go through
    # tests/gpu_test_device.h, which is itself listed here.
    "tests/gpu_test_device.h",
)

# A test may drive a real device — several build a Noop backend and hand real
# handles to a Mesh. What a test may NOT do is reintroduce the mixing this
# check exists to prevent, so tests are reported separately rather than
# ignored: a growing list here is the early warning.
TEST_PREFIX = "tests/"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]bgfx/', re.M)
# bx is bgfx's MATH library and is a separate question with its own schedule
# (docs/rhi/evidence-coupling.md 2.1). Explicitly not checked here, so that
# nobody reads this script as having an opinion about it.


def tracked_sources() -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(REPO), "ls-files", "src", "include", "tests"],
        capture_output=True, text=True, check=False,
    ).stdout.split()
    return [p for p in out if p.endswith((".h", ".hpp", ".cpp", ".mm", ".c"))]


def main() -> int:
    offenders: list[str] = []
    test_users: list[str] = []

    for rel in tracked_sources():
        if rel.startswith(ALLOWED_PREFIXES):
            continue
        text = (REPO / rel).read_text(errors="ignore")
        if not INCLUDE_RE.search(text):
            continue
        (test_users if rel.startswith(TEST_PREFIX) else offenders).append(rel)

    if test_users:
        print("note: tests including bgfx directly (allowed, but prefer "
              "tests/gpu_test_device.h):")
        for t in sorted(test_users):
            print(f"  {t}")

    if offenders:
        print("\nFAIL: these files include a graphics API outside the renderer:\n")
        for o in sorted(offenders):
            print(f"  {o}")
        print(
            "\nThe asset path must not name a backend. Use render/gpu.h:\n"
            "  gpu::copy / gpu::alloc          stage bytes (safe on any thread)\n"
            "  gpu::createVertexBuffer / ...   turn a blob into a handle (main only)\n"
            "  gpu::destroy                    release one\n"
            "\nIf you are inside src/render/ and genuinely need the backend "
            "handle, include render/gpu_bgfx.h and convert with gpu::toBgfx().\n"
            "See docs/rhi/phases.md G1 and src/render/gpu.h.\n"
        )
        return 1

    print("gpu seam: clean — no graphics API outside the renderer")
    return 0


if __name__ == "__main__":
    sys.exit(main())
