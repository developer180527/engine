#!/usr/bin/env python3
"""Architectural invariants, checked mechanically.

WHY THIS EXISTS
---------------
`engine_doctor.py` checks the DOCUMENT contract — does a doc exist, is it stale,
does its tier have evidence. It deliberately says nothing about the code's
SHAPE. The shape rules live in prose instead, scattered across info.md files:

    src/core/info.md      "Nothing here may include renderer, ECS, or editor
                           headers."
    src/render/world/     "Being GPU-free *and* runtime-free is the whole
                           point."
    extension-model.md    a reordered API group "would keep every size intact
                           and still break every Kit"

Prose rules decay in one specific way: not by being repealed, but by one
`#include` added to fix one compile error, in a file nobody thought of as
belonging to that layer. That is exactly how the GPU seam drifted in four
places before `check_gpu_seam.py` existed, and the same reasoning applies to
every other boundary this engine claims to have.

So: the same treatment, generalised. Each rule below is a claim some document
already makes, turned into something a machine decides.

THE RATCHET, AND WHY THERE IS A BASELINE
----------------------------------------
A new checker that reports 40 pre-existing violations gets ignored, and then it
is worse than nothing — it is a red light everyone has learned to walk past. So
findings that exist TODAY are recorded in `scripts/audit_baseline.json` and the
gate fails only on NEW ones. Existing debt stays visible (it is listed in every
report, and `--list-debt` prints nothing else) but it does not block work.

Signatures deliberately exclude line numbers: moving a violation around a file
must not read as "fixed one, found another".

WHAT THIS TOOL CANNOT DO, STATED SO NOBODY TRUSTS IT TOO FAR
------------------------------------------------------------
It reads text. It does not parse C++, expand macros, or follow `#ifdef`. It
cannot judge whether a boundary SHOULD exist, whether an abstraction earns its
weight, or whether a 900-line file is too big. Those need a human, and the
report says so rather than implying the green light covers them.

Run:
    python3 scripts/engine_audit.py                 # human report
    python3 scripts/engine_audit.py --check         # CI/farm gate (new only)
    python3 scripts/engine_audit.py --json out.json
    python3 scripts/engine_audit.py --sqlite farm.db --run-id 7
    python3 scripts/engine_audit.py --update-baseline
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
BASELINE = Path(__file__).resolve().parent / "audit_baseline.json"

# Quoted includes resolve against these, in this order — mirrors the
# target_include_directories in src/CMakeLists.txt. If that list changes, this
# one must too; a wrong resolution shows up as a MISSING edge, which is the
# quiet failure direction, so RULE LAYER-03 double-reports unresolved includes.
INCLUDE_DIRS = ["src", "src/core", "src/components", "src/render", "include", "."]

SRC_EXT = (".h", ".hpp", ".cpp", ".inl", ".c", ".mm")


# ── Findings ─────────────────────────────────────────────────────────────────
@dataclass(frozen=True)
class Finding:
    rule: str          # stable id, e.g. "LAYER-01"
    subject: str       # the path (or symbol) the finding is about
    detail: str        # one line, human-facing
    severity: str = "error"

    @property
    def signature(self) -> str:
        """Stable across line moves and reworded messages."""
        return f"{self.rule}|{self.subject}"


@dataclass
class Rule:
    id: str
    title: str
    source: str        # the document that already states this rule
    why: str           # what breaks when it is violated
    findings: list[Finding] = field(default_factory=list)


# ── Tree access ──────────────────────────────────────────────────────────────
def git(*args: str) -> str:
    return subprocess.run(["git", "-C", str(REPO), *args],
                          capture_output=True, text=True, check=False).stdout


def tracked(*globs: str) -> list[str]:
    """Tracked files only.

    Kits/ and fps_shooter/ are gitignored working trees; auditing them would
    report violations in code this repo does not own and cannot fix.
    """
    out = git("ls-files", *globs).split("\n")
    return [p for p in out if p]


def read(rel: str) -> str:
    try:
        return (REPO / rel).read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


_INC_Q = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)
_INC_A = re.compile(r'^\s*#\s*include\s*<([^>]+)>', re.M)
# Comments are prose, and this codebase discusses its own includes constantly
# ("nothing here includes bgfx"). Counting a sentence as an include would make
# the strongest documentation the loudest violation — check_std_includes.py
# learned this the hard way, so the same stripping happens here.
_STRIP = re.compile(r'/\*.*?\*/|//[^\n]*', re.S)


def includes(rel: str) -> tuple[list[str], list[str]]:
    text = _STRIP.sub(lambda m: re.sub(r'[^\n]', ' ', m.group(0)), read(rel))
    return _INC_Q.findall(text), _INC_A.findall(text)


def resolve(inc: str) -> str | None:
    for d in INCLUDE_DIRS:
        p = f"{d}/{inc}" if d != "." else inc
        if (REPO / p).exists():
            return p.replace("\\", "/")
    return None


def module_of(path: str | None) -> str | None:
    """src/runtime/input/x.h -> src/runtime. Sub-areas roll up to the module."""
    if not path or not path.startswith("src/"):
        return None
    parts = path.split("/")
    return f"src/{parts[1]}" if len(parts) > 2 else None


THIRD_PARTY = {
    "bgfx": "bgfx", "bimg": "bgfx",          # bx is MATH, deliberately not here
    "imgui": "imgui", "ImGuizmo": "imgui",
    "flecs": "flecs",
    "Jolt": "jolt", "lua": "lua", "ozz": "ozz",
    "GLFW": "glfw", "SDL": "sdl", "assimp": "assimp",
}


def third_party_of(inc: str) -> str | None:
    head = inc.split("/")[0].replace(".h", "").replace(".hpp", "")
    return THIRD_PARTY.get(head)


# ── LAYER rules ──────────────────────────────────────────────────────────────
def rule_core_purity() -> Rule:
    r = Rule("LAYER-01", "src/core stays dependency-light",
             "src/core/info.md — \"Nothing here may include renderer, ECS, or "
             "editor headers.\"",
             "core is included by every other subsystem, so anything core "
             "depends on becomes a dependency of the whole engine — including "
             "of a headless server and of the cook tools.")
    for rel in tracked("src/core/*"):
        if not rel.endswith(SRC_EXT):
            continue
        q, a = includes(rel)
        for inc in a:
            tp = third_party_of(inc)
            # bx/math.h is the MATH library, not the renderer. check_gpu_seam.py
            # makes the same exclusion for the same reason.
            if tp in ("bgfx", "imgui", "flecs"):
                r.findings.append(Finding(r.id, f"{rel}:<{inc}>",
                                          f"{rel} includes {tp} (<{inc}>)"))
        for inc in q:
            tgt = module_of(resolve(inc))
            if tgt in ("src/render", "src/editor", "src/runtime"):
                r.findings.append(Finding(r.id, f"{rel}:{inc}",
                                          f"{rel} includes {tgt} ({inc})"))
    return r


def rule_editor_isolation() -> Rule:
    r = Rule("LAYER-02", "the editor is a leaf",
             "docs/process/engineering-standards.md §7 — src/editor is "
             "\"deliberately contained (nothing else depends on it)\".",
             "an editor dependency in engine code puts ImGui and editor state "
             "into the runtime, the player and the cook tools.")
    for rel in tracked("src/*"):
        if not rel.endswith(SRC_EXT) or rel.startswith("src/editor/"):
            continue
        q, a = includes(rel)
        for inc in q:
            if module_of(resolve(inc)) == "src/editor":
                r.findings.append(Finding(r.id, f"{rel}:{inc}",
                                          f"{rel} includes src/editor ({inc})"))
        for inc in a:
            if third_party_of(inc) == "imgui":
                r.findings.append(Finding(r.id, f"{rel}:<{inc}>",
                                          f"{rel} includes imgui (<{inc}>)"))
    return r


def rule_render_world_purity() -> Rule:
    r = Rule("LAYER-04", "src/render/world is GPU-free and runtime-free",
             "src/render/world/info.md — \"Being GPU-free *and* runtime-free is "
             "the whole point.\"",
             "this layer decides visibility and batching for a frame; a runtime "
             "or GPU dependency here is what stops a server culling without a "
             "window, and stops the layer being testable without a device.")
    for rel in tracked("src/render/world/*"):
        if not rel.endswith(SRC_EXT):
            continue
        q, a = includes(rel)
        for inc in q:
            if (resolve(inc) or "").startswith("src/runtime/"):
                r.findings.append(Finding(r.id, f"{rel}:{inc}",
                                          f"{rel} includes the runtime ({inc})"))
        for inc in a:
            if third_party_of(inc) == "bgfx":
                r.findings.append(Finding(r.id, f"{rel}:<{inc}>",
                                          f"{rel} includes bgfx (<{inc}>)"))
    return r


def module_edges() -> dict[str, set[str]]:
    edges: dict[str, set[str]] = {}
    for rel in tracked("src/*"):
        if not rel.endswith(SRC_EXT):
            continue
        src = module_of(rel)
        if not src:
            continue
        q, _ = includes(rel)
        for inc in q:
            dst = module_of(resolve(inc))
            if dst and dst != src:
                edges.setdefault(f"{src} -> {dst}", set()).add(rel)
    return edges


def rule_declared_edges() -> Rule:
    r = Rule("LAYER-03", "no new coupling between modules without a decision",
             "docs/plans/subsystem-audit.md §2 — the dependency graph is an "
             "argument about order, so growing it silently invalidates it.",
             "each new module->module edge widens the blast radius of a change "
             "and is usually added to fix one compile error, never revisited.")
    for edge, files in sorted(module_edges().items()):
        r.findings.append(Finding(r.id, edge,
                                  f"{edge} ({len(files)} file(s))", "info"))
    return r


# ── ABI rules ────────────────────────────────────────────────────────────────
_TABLE_BLOCK = re.compile(
    r'typedef struct EngineApiTableV1 \{(.*?)\} EngineApiTableV1;', re.S)
_TABLE_FIELD = re.compile(r'^\s*(EngineApi\w+V\d)\s+(\w+)\s*;', re.M)
_FROZEN = re.compile(r'ENGINE_API_FROZEN\(\s*(\w+)\s*,\s*(\d+)\s*\)')
_GROUP_AT = re.compile(r'ENGINE_API_GROUP_AT\(\s*(\w+)\s*,\s*(\d+)\s*\)')
_CLIENT_ROW = re.compile(r'\{\s*(EAPI_\w+)\s*,\s*t->(\w+)\.version\s*,\s*(ENGINE_API_\w+_V)')
_COMPAT_ROW = re.compile(r'\{\s*"(\w+)"\s*,\s*(\d+)\s*,\s*(\d+)\s*\}')


def api_groups() -> tuple[list[tuple[str, str]], dict[str, int], dict[str, int]]:
    hdr = read("include/engine/engine_api_table.h")
    block = _TABLE_BLOCK.search(hdr)
    fields = _TABLE_FIELD.findall(block.group(1)) if block else []
    sizes = {t: int(n) for t, n in _FROZEN.findall(hdr)}
    offs = {f: int(n) for f, n in _GROUP_AT.findall(hdr)}
    return fields, sizes, offs


def rule_abi_group_wiring() -> Rule:
    r = Rule("ABI-01", "every API group is frozen, placed and guarded",
             "docs/architecture/extension-model.md §1.3 + "
             "include/engine/engine_api_table.h's compatibility contract.",
             "a group with no ENGINE_API_GROUP_AT can move when an earlier "
             "group changes size, and every already-built kit then reads the "
             "wrong function pointers. A group with no client guard row is "
             "called without a version check at all.")
    fields, sizes, offs = api_groups()
    client = read("include/engine/engine_api_client.h")
    impl = read("src/runtime/scripting/engine_api_table.cpp")
    rows = {f: (e, m) for e, f, m in _CLIENT_ROW.findall(client)}
    for typ, fld in fields:
        if typ not in sizes:
            r.findings.append(Finding(r.id, f"{fld}:frozen",
                                      f"group '{fld}' ({typ}) has no ENGINE_API_FROZEN size"))
        if fld not in offs:
            r.findings.append(Finding(r.id, f"{fld}:group_at",
                                      f"group '{fld}' has no ENGINE_API_GROUP_AT offset"))
        if fld not in rows:
            r.findings.append(Finding(r.id, f"{fld}:client_row",
                                      f"group '{fld}' has no row in engine_api_client.h's "
                                      f"version table — it is callable with no guard"))
        else:
            macro = rows[fld][1]
            if macro not in impl:
                r.findings.append(Finding(r.id, f"{fld}:impl_row",
                                          f"group '{fld}' guards on {macro}, which the host "
                                          f"table (engine_api_table.cpp) never sets"))
    return r


def rule_abi_offsets_tile() -> Rule:
    r = Rule("ABI-02", "API group offsets tile with no gap and no overlap",
             "include/engine/engine_api_table.h — offsets are frozen "
             "individually; this checks they still describe one packed table.",
             "a gap means a pinned offset disagrees with the real struct "
             "layout, so the asserts pass on this compiler and a kit built "
             "with different padding reads a neighbouring group.")
    fields, sizes, offs = api_groups()
    placed = [(fld, offs[fld], sizes.get(typ, 0))
              for typ, fld in fields if fld in offs]
    order = [f for f, _, _ in placed]
    if order != sorted(order, key=lambda f: offs[f]):
        r.findings.append(Finding(r.id, "field_order",
                                  "struct field order does not match ENGINE_API_GROUP_AT "
                                  "offset order"))
    for (f1, o1, s1), (f2, o2, _) in zip(placed, placed[1:]):
        if o1 + s1 != o2:
            r.findings.append(Finding(
                r.id, f"{f1}->{f2}",
                f"'{f1}' at {o1} + size {s1} = {o1 + s1}, but '{f2}' is pinned at "
                f"{o2} ({'gap' if o2 > o1 + s1 else 'overlap'})"))
    return r


def rule_abi_compat_coverage() -> Rule:
    r = Rule("ABI-03", "the compatibility test covers every API group",
             "tests/api_abi_compat_test.cpp §1 — \"a reordered group would keep "
             "every size intact and still break every kit\".",
             "the test's frozen[] list is a hand-maintained copy of the "
             "header's offsets. A group missing from it is a group whose "
             "placement no runtime test defends.")
    fields, sizes, offs = api_groups()
    test = read("tests/api_abi_compat_test.cpp")
    listed = {n: (int(o), int(s)) for n, o, s in _COMPAT_ROW.findall(test)}
    for typ, fld in fields:
        if fld not in listed:
            r.findings.append(Finding(r.id, fld,
                                      f"group '{fld}' (offset {offs.get(fld, '?')}) is not in "
                                      f"api_abi_compat_test's frozen[] list"))
        else:
            o, s = listed[fld]
            if fld in offs and o != offs[fld]:
                r.findings.append(Finding(r.id, f"{fld}:offset",
                                          f"test says '{fld}' is at {o}, header pins {offs[fld]}"))
            if typ in sizes and s != sizes[typ]:
                r.findings.append(Finding(r.id, f"{fld}:size",
                                          f"test says '{fld}' is {s} bytes, header freezes "
                                          f"{sizes[typ]}"))
    return r


_HASH_TYPE = re.compile(r'ENGINE_ABI_HASH_TYPE\((\w+)\)')
_STRUCT = re.compile(r'^\s*(?:struct|class)\s+(\w+)\s*(?:final\s*)?\{', re.M)


def rule_component_hash_membership() -> Rule:
    r = Rule("ABI-04", "every kit-visible component is in componentLayoutHash",
             "tests/component_abi_test.cpp — \"A component added to the engine "
             "and NOT added to componentLayoutHash is invisible to the gate: "
             "kits could disagree about it forever.\"",
             "the hash is what makes ModuleLibrary::load refuse a stale kit. A "
             "component outside it can change shape while kits keep reading the "
             "old layout out of live world memory.")
    hashed = set(_HASH_TYPE.findall(read("include/engine/game_module.h")))
    if not hashed:
        r.findings.append(Finding(r.id, "parse",
                                  "could not find ENGINE_ABI_HASH_TYPE entries in "
                                  "game_module.h — this rule is not checking anything"))
        return r
    q, _ = includes("include/engine/components.h")
    for inc in q:
        path = resolve(inc)
        if not path:
            continue
        declared = set(_STRUCT.findall(read(path)))
        if declared and not (declared & hashed):
            r.findings.append(Finding(
                r.id, inc,
                f"components.h re-exports {inc} (declares "
                f"{', '.join(sorted(declared)[:3])}) but none of its types are in "
                f"componentLayoutHash()"))
    return r


# ── Test-hygiene rules ───────────────────────────────────────────────────────
_FUZZ_TARGET = re.compile(r'engine_fuzz_target\(\s*([\w]+)\s+([\w]+)')


def rule_fuzz_corpus() -> Rule:
    r = Rule("TEST-01", "every fuzz target has a corpus, and vice versa",
             "docs/plans/automated-testing-soak-fuzz-plan.md §1.4 — the seed "
             "corpus is how a found bug stays found.",
             "a target with no corpus has no regression lane: the bug it found "
             "once is not pinned, and the gating _regress run tests nothing.")
    cmake = read("tests/CMakeLists.txt")
    targets = _FUZZ_TARGET.findall(cmake)
    for tgt, domain in targets:
        d = REPO / "tests" / "fuzz" / "corpus" / domain
        if not d.is_dir():
            r.findings.append(Finding(r.id, domain,
                                      f"fuzz target {tgt} declares domain '{domain}' with no "
                                      f"corpus directory"))
        elif not list(d.glob("*.seeds")):
            r.findings.append(Finding(r.id, f"{domain}:seeds",
                                      f"corpus/{domain} has no .seeds file — the regress lane "
                                      f"replays nothing"))
    known = {d for _, d in targets}
    corpus_root = REPO / "tests" / "fuzz" / "corpus"
    if corpus_root.is_dir():
        for d in sorted(corpus_root.iterdir()):
            if d.is_dir() and d.name not in known:
                r.findings.append(Finding(r.id, f"orphan:{d.name}",
                                          f"corpus/{d.name} has no engine_fuzz_target — dead "
                                          f"seeds nothing replays", "warn"))
    return r


def rule_tests_registered() -> Rule:
    r = Rule("TEST-02", "every test file is registered with ctest",
             "docs/process/engineering-standards.md §4.1 — tests belong to a "
             "lane; an unregistered file is in none.",
             "a test nobody runs is worse than no test: it reads as coverage. "
             "The ctest lane `module_abi_conformance` existed for weeks without "
             "running once for exactly this reason.")
    cmake = read("tests/CMakeLists.txt")
    for rel in tracked("tests/*.cpp", "tests/*.c"):
        stem = Path(rel).stem
        if not re.search(rf'\b{re.escape(stem)}\b', cmake):
            r.findings.append(Finding(r.id, rel,
                                      f"{rel} is not named anywhere in tests/CMakeLists.txt"))
    return r


# ── Determinism + header hygiene ─────────────────────────────────────────────
# The TUs that run inside the fixed step. Wall-clock and RNG here decide
# simulation state, and the determinism gate can only catch that when a tier
# happens to exercise the path — which is how hid::nowNs() survived in the tick.
SIM_TUS = ("src/runtime/runtime_sim.cpp", "src/runtime/sim_command.cpp",
           "src/runtime/sim_intent.cpp", "src/runtime/move_compose.cpp",
           "src/runtime/take.cpp", "src/runtime/sim_hash.h",
           "src/runtime/sim_command.h", "src/runtime/sim_intent.h")
NONDETERMINISM = (
    (re.compile(r'\bnowNs\s*\('), "wall-clock (hid::nowNs)"),
    (re.compile(r'std::chrono::[a-z_]*clock'), "wall-clock (std::chrono)"),
    (re.compile(r'\brand\s*\(\)|\bmt19937\b|\brandom_device\b'), "RNG"),
    (re.compile(r'\bstd::time\s*\('), "wall-clock (std::time)"),
)


def rule_sim_determinism() -> Rule:
    r = Rule("DET-01", "the fixed step reads no clock and no RNG",
             "src/runtime/docs/info.md — the gate's own limits table lists "
             "\"hid::nowNs() inside the fixed step\" as an open hazard.",
             "anything the tick reads that is not in the recorded input makes "
             "a replay, a rollback and a lockstep peer disagree — and the "
             "determinism gate only sees it if a tier happens to drive it.")
    for rel in SIM_TUS:
        text = _STRIP.sub("", read(rel))
        for rx, what in NONDETERMINISM:
            if rx.search(text):
                r.findings.append(Finding(r.id, f"{rel}:{what}",
                                          f"{rel} reads {what} inside the simulation path"))
    return r


# C-callable headers. A kit compiles these with a C compiler, or with a
# different C++ standard library, so anything they pull in is part of the ABI.
C_ABI_HEADERS = ("include/engine/engine_api.h",
                 "include/engine/engine_api_table.h",
                 "include/engine/engine_api_client.h",
                 "include/engine/addon_protocol.h",
                 "include/engine/contract.h")


def rule_c_abi_header_purity() -> Rule:
    r = Rule("HDR-01", "the C ABI headers pull in nothing of ours",
             "docs/architecture/extension-model.md — the C ABI exists so a kit "
             "needs no engine headers and no matching toolchain.",
             "a third-party or internal include in these headers drags the "
             "engine's build configuration into every kit, which is the "
             "coupling the flat C surface was built to remove.")
    for rel in C_ABI_HEADERS:
        if not (REPO / rel).exists():
            continue
        q, a = includes(rel)
        for inc in a:
            tp = third_party_of(inc)
            if tp:
                r.findings.append(Finding(r.id, f"{rel}:<{inc}>",
                                          f"{Path(rel).name} includes {tp} (<{inc}>)"))
        for inc in q:
            tgt = resolve(inc)
            if tgt and tgt.startswith("src/"):
                r.findings.append(Finding(r.id, f"{rel}:{inc}",
                                          f"{Path(rel).name} includes engine-internal {inc}"))
    return r


def rule_doc_coverage() -> Rule:
    r = Rule("DOC-01", "every directory of code is covered by some document",
             "docs/process/engineering-standards.md §1 — staleness is checked "
             "against `covers:`, so uncovered code is code the contract cannot "
             "see.",
             "a subsystem no doc claims has no tier, no staleness check and no "
             "test evidence requirement — it is invisible to the whole "
             "maturity ladder rather than rated low by it.")
    covers: list[str] = []
    for rel in tracked("*.md"):
        text = read(rel)
        if not text.startswith("---"):
            continue
        fm = text.split("---", 2)[1] if text.count("---") >= 2 else ""
        if "covers:" not in fm:
            continue
        for line in fm.split("covers:", 1)[1].splitlines()[1:]:
            m = re.match(r'\s+-\s+(\S+)', line)
            if not m:
                break
            covers.append(m.group(1).rstrip("/"))
    dirs = {str(Path(p).parent) for p in tracked("src/*") if p.endswith(SRC_EXT)}
    for d in sorted(dirs):
        if not any(d == c or d.startswith(c + "/") for c in covers):
            r.findings.append(Finding(r.id, d, f"{d} is covered by no document"))
    return r


RULES = (rule_core_purity, rule_editor_isolation, rule_declared_edges,
         rule_render_world_purity, rule_abi_group_wiring, rule_abi_offsets_tile,
         rule_abi_compat_coverage, rule_component_hash_membership,
         rule_fuzz_corpus, rule_tests_registered, rule_sim_determinism,
         rule_c_abi_header_purity, rule_doc_coverage)

# The checks that already exist as their own scripts. The farm wants ONE entry
# point, and duplicating these here would mean two definitions of the same rule
# drifting apart — so they are invoked, not reimplemented.
EXTERNAL = (("EXT-GPU", "scripts/check_gpu_seam.py",
             "nothing outside the renderer names a graphics API"),
            ("EXT-STD", "scripts/check_std_includes.py",
             "no std:: use without the header that provides it"),
            ("EXT-DOC", "scripts/engine_doctor.py",
             "the document contract (tiers, staleness, bug ledger)"))


def run_external() -> list[tuple[str, str, int, str]]:
    out = []
    for rid, script, title in EXTERNAL:
        if not (REPO / script).exists():
            continue
        args = [sys.executable, str(REPO / script)]
        if script.endswith("engine_doctor.py"):
            args.append("check")
        p = subprocess.run(args, capture_output=True, text=True, check=False)
        out.append((rid, title, p.returncode, (p.stdout + p.stderr).strip()))
    return out


# ── Baseline ─────────────────────────────────────────────────────────────────
def load_baseline() -> dict:
    if not BASELINE.exists():
        return {"findings": {}}
    try:
        return json.loads(BASELINE.read_text())
    except (OSError, json.JSONDecodeError):
        return {"findings": {}}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero on findings not in the baseline")
    ap.add_argument("--json", metavar="PATH", help="write the full report as JSON")
    ap.add_argument("--sqlite", metavar="DB",
                    help="append the report to a farm results database")
    ap.add_argument("--run-id", type=int, default=None,
                    help="run(id) to attach SQLite rows to (farm schema §5.2)")
    ap.add_argument("--update-baseline", action="store_true",
                    help="accept every current finding as existing debt")
    ap.add_argument("--list-debt", action="store_true",
                    help="print only the accepted (baseline) findings")
    ap.add_argument("--with-external", action="store_true",
                    help="also run the doctor, gpu-seam and std-include checks")
    args = ap.parse_args()

    rules = [fn() for fn in RULES]
    baseline = load_baseline()
    accepted = {r: set(s) for r, s in baseline.get("findings", {}).items()}

    if args.update_baseline:
        payload = {
            "generated": datetime.datetime.now(datetime.timezone.utc)
                                 .isoformat(timespec="seconds"),
            "commit": git("rev-parse", "HEAD").strip(),
            "note": "Accepted architectural debt. The gate fails on findings "
                    "NOT listed here; shrinking this file is the cleanup, and "
                    "nothing may be added to it without a reason in the commit.",
            "findings": {r.id: sorted(f.signature for f in r.findings)
                         for r in rules if r.findings},
        }
        BASELINE.write_text(json.dumps(payload, indent=2) + "\n")
        n = sum(len(v) for v in payload["findings"].values())
        print(f"engine_audit: baseline updated — {n} finding(s) accepted as debt")
        return 0

    new: list[Finding] = []
    old: list[Finding] = []
    for r in rules:
        for f in r.findings:
            (old if f.signature in accepted.get(r.id, ()) else new).append(f)

    if args.list_debt:
        for f in old:
            print(f"  {f.rule}  {f.detail}")
        print(f"\n{len(old)} accepted finding(s)")
        return 0

    # ── Report ──────────────────────────────────────────────────────────────
    print("engine_audit — architectural invariants\n")
    for r in rules:
        # No severity filter: an informational finding that is NOT baselined is
        # still something that appeared since the baseline, which is the whole
        # question this tool answers. Filtering them here (an earlier version
        # did) made every rule print `ok` while the summary counted 56 — a
        # report that disagrees with its own total teaches you to trust neither.
        fresh = [f for f in r.findings if f.signature not in accepted.get(r.id, ())]
        held = [f for f in r.findings if f.signature in accepted.get(r.id, ())]
        if r.id == "LAYER-03":
            mark = "ok   " if not fresh else "NEW  "
            print(f"  {mark} {r.id}  {r.title} "
                  f"({len(r.findings)} edges, {len(held)} baselined"
                  + (f", {len(fresh)} NEW" if fresh else "") + ")")
        else:
            mark = "NEW  " if fresh else ("debt " if held else "ok   ")
            print(f"  {mark} {r.id}  {r.title}"
                  + (f" — {len(fresh)} new" if fresh else "")
                  + (f", {len(held)} accepted" if held else ""))
        for f in fresh:
            print(f"          {f.detail}")

    if args.with_external:
        print()
        for rid, title, code, output in run_external():
            print(f"  {'ok   ' if code == 0 else 'FAIL '} {rid}  {title}")
            if code != 0:
                for line in output.splitlines()[:12]:
                    print(f"          {line}")

    print(f"\n{len(new)} new finding(s), {len(old)} accepted as existing debt.")
    print("This tool reads text: it cannot judge whether a boundary should "
          "exist,\nwhether an abstraction earns its weight, or whether a file "
          "is too big.")

    if args.json or args.sqlite:
        report = {
            "generated": datetime.datetime.now(datetime.timezone.utc)
                                 .isoformat(timespec="seconds"),
            "commit": git("rev-parse", "HEAD").strip(),
            "rules": [{"id": r.id, "title": r.title, "source": r.source,
                       "why": r.why,
                       "findings": [{"subject": f.subject, "detail": f.detail,
                                     "severity": f.severity,
                                     "new": f.signature not in accepted.get(r.id, ())}
                                    for f in r.findings]}
                      for r in rules],
            "new_count": len(new), "accepted_count": len(old),
        }
        if args.json:
            Path(args.json).write_text(json.dumps(report, indent=2) + "\n")
        if args.sqlite:
            write_sqlite(args.sqlite, report, args.run_id)

    if args.check and new:
        print(f"\nFAIL: {len(new)} finding(s) not in {BASELINE.name}. Fix them, "
              f"or record the decision\nwith --update-baseline and say why in "
              f"the commit message.")
        return 1
    return 0


def write_sqlite(db_path: str, report: dict, run_id: int | None) -> None:
    """Append to the farm database (docs/plans/automated-testing-soak-fuzz-plan.md §5).

    Two tables, mirroring §5.3's dedup-at-the-source shape: a finding is
    identified by its signature and carries first/last seen, so the farm can
    answer "is this new?" without diffing reports.
    """
    import sqlite3
    con = sqlite3.connect(db_path)
    con.executescript("""
        CREATE TABLE IF NOT EXISTS audit_run (
            id         INTEGER PRIMARY KEY,
            run_id     INTEGER,            -- farm run(id), when invoked by soakd
            commit_sha TEXT NOT NULL,
            at_utc     TEXT NOT NULL,
            new_count  INTEGER NOT NULL,
            debt_count INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS audit_finding (
            signature   TEXT PRIMARY KEY,  -- rule|subject, stable across line moves
            rule        TEXT NOT NULL,
            subject     TEXT NOT NULL,
            detail      TEXT NOT NULL,
            severity    TEXT NOT NULL,
            first_seen  TEXT NOT NULL,
            last_seen   TEXT NOT NULL,
            first_commit TEXT NOT NULL,
            last_commit  TEXT NOT NULL,
            hit_count   INTEGER NOT NULL DEFAULT 1,
            baselined   INTEGER NOT NULL
        );
    """)
    con.execute("INSERT INTO audit_run (run_id, commit_sha, at_utc, new_count, "
                "debt_count) VALUES (?,?,?,?,?)",
                (run_id, report["commit"], report["generated"],
                 report["new_count"], report["accepted_count"]))
    for rule in report["rules"]:
        for f in rule["findings"]:
            sig = f"{rule['id']}|{f['subject']}"
            con.execute("""
                INSERT INTO audit_finding (signature, rule, subject, detail,
                    severity, first_seen, last_seen, first_commit, last_commit,
                    baselined)
                VALUES (?,?,?,?,?,?,?,?,?,?)
                ON CONFLICT(signature) DO UPDATE SET
                    last_seen=excluded.last_seen,
                    last_commit=excluded.last_commit,
                    detail=excluded.detail,
                    baselined=excluded.baselined,
                    hit_count=audit_finding.hit_count+1
            """, (sig, rule["id"], f["subject"], f["detail"], f["severity"],
                  report["generated"], report["generated"], report["commit"],
                  report["commit"], 0 if f["new"] else 1))
    con.commit()
    con.close()


if __name__ == "__main__":
    sys.exit(main())
