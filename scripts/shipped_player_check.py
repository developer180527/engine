#!/usr/bin/env python3
"""Package a game with engine_build, twice, and check what ships (WO-088).

A shipped game links its providers statically, chosen by engine_build
(WO-086). A development build loads every provider there is. If the tests
only ever ran the development build, the build players get would be the one
nobody tests. So this, on every CI leg:

  1. scaffolds a project (engine_project create, template basic3d) twice:
       default   the engine's choices: forward-bgfx, jolt, lua, miniaudio
       headless  a game with no renderer and no audio (renderer none, audio none)
  2. packages each with engine_build into its own dist/, timed;
  3. checks each dist/providers.lock.json names what was chosen;
  4. reads each packaged player's symbols: every chosen provider is in it, no
     other provider is, and the resolver is not (a shipped player resolves
     nothing);
  5. runs the headless package headless for 120 frames: it must start, step
     and exit cleanly; and runs the default package headless for 1 frame: it
     must refuse, by name, because its renderer needs a display.
  6. conformance inside the player (WO-093): the default package is built with
     `engine_build --conformance`, and `engine_player --conformance audio` runs
     the audio suite against the provider that player linked; then again with
     --mutate (play claims a voice for an invalid sound), which must FAIL. The
     headless package is built without it, as every game is: its symbols must
     not contain the suite, and its player must refuse --conformance by name.

"headless" stands where WO-088 says "server": engine_build refuses
--flavour server because no server player exists yet, so the second selection
is a client player with a server's choices.

  --mutation <binary>  check a player that carries EVERY provider (the
                       development build's) against the headless lock: the
                       check must fail. ctest runs this, so the failure path is
                       exercised, not only the pass.

Run: python3 scripts/shipped_player_check.py --build <dev build dir> --work <dir>
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_no_cook_stack import symbol_lines  # noqa: E402  (nm, or MSVC's link map)

EXE = ".exe" if os.name == "nt" else ""

# A symbol each provider's code is sure to leave in a binary that links it, as
# a pattern over mangled names (nm) or MSVC map lines. A NEW provider adds a
# line here; a provider with no line is reported, never passed silently.
MARKERS = {
    ("physics", "jolt"):           r"JoltPlugin",
    ("physics", "none"):           r"NullPhysicsPlugin",
    ("scripting", "lua"):          r"LuaScriptPlugin",
    ("scripting", "none"):         r"NullScriptPlugin",
    ("audio", "miniaudio"):        r"(?<!Null)AudioPlugin",
    ("audio", "none"):             r"NullAudioPlugin",
    ("renderer", "forward-bgfx"):  r"ForwardPipeline",
    # No marker: NullRenderer is in every player whatever was chosen, because
    # the runtime installs it from construction so a call before init() is safe
    # (runtime.cpp, EngineRuntime::EngineRuntime). Its presence says nothing
    # about the choice; when chosen it is checked through the lock and the run.
    ("renderer", "none"):          None,
}
# Never in a shipped player (DR-0032): the resolver (engine_build resolved),
# and the manifest reader with it.
NEVER = {"the provider resolver (project/provider_resolve.h)": r"provsel"}
# In a test package's player and in no other (WO-093, engine_build --conformance).
CONFORMANCE = r"engine_audio_conformance_run"

SELECTIONS = {
    "default":  None,                                     # the engine's defaults
    "headless": {"renderer": "none", "audio": "none"},
}
# Built with engine_build --conformance: a test package (WO-093). The other is
# built as a game is, so the check that a game's player lacks the suite runs too.
WITH_CONFORMANCE = {"default"}


def check_symbols(binary: Path, lock: dict) -> list[str]:
    """Problems with `binary` against `lock`; empty when it is right."""
    lines, source = symbol_lines(str(binary))
    if not lines:
        return [f"{binary}: no symbols to check ({source}); nothing read is not a pass"]
    text = [l for l in lines if "??_C@" not in l]   # MSVC string literals are not code
    chosen = {(p["kind"], p["name"]) for p in lock["providers"]}
    problems = []
    for key in chosen:
        if key not in MARKERS:
            problems.append(f"{key[0]} '{key[1]}' was chosen but has no marker in this script")
    for key, pat in MARKERS.items():
        if pat is None:
            continue   # always linked (see MARKERS); not a signal of the choice
        rx = re.compile(pat)
        present = any(rx.search(l) for l in text)
        if key in chosen and not present:
            problems.append(f"the chosen {key[0]} provider '{key[1]}' is not in the player")
        if key not in chosen and present:
            problems.append(f"the {key[0]} provider '{key[1]}' is in the player, and this game did not choose it")
    for what, pat in NEVER.items():
        if any(re.search(pat, l) for l in text):
            problems.append(f"{what} is in the player")
    has_conf = any(re.search(CONFORMANCE, l) for l in text)
    if lock.get("conformance") and not has_conf:
        problems.append("the conformance suite is not in this test package's player")
    if not lock.get("conformance") and has_conf:
        problems.append("the conformance suite is in the player, and this package was built as a game's")
    return problems


def conformance_runs(player: Path, dist: Path, name: str, lock: dict, failures: list[str]) -> str:
    """WO-093: the suites inside the player, or their absence from a game's."""
    if not lock.get("conformance"):
        r = run([player, "--conformance", "audio"], cwd=dist)
        if r.returncode == 0 or "only in players built to test" not in (r.stdout + r.stderr):
            failures.append(f"{name}: a game's player should refuse --conformance by name "
                            f"(exit {r.returncode}):\n{r.stdout[-1500:]}{r.stderr[-1500:]}")
            return "FAILED: a game's player ran it"
        return "absent, refused by name"
    names = {p["kind"]: p["name"] for p in lock["providers"]}
    r = run([player, "--conformance", "audio"], cwd=dist)
    out = r.stdout + r.stderr
    passed = r.returncode == 0 and "every check passed" in out
    if not passed:
        failures.append(f"{name}: the audio suite failed against the linked provider "
                        f"'{names.get('audio')}' (exit {r.returncode}):\n{out[-4000:]}")
    m = run([player, "--conformance", "audio", "--mutate"], cwd=dist)
    mout = m.stdout + m.stderr
    caught = m.returncode == 1 and "FAILED" in mout and "unknown sound yields NO_VOICE" in mout
    if not caught:
        failures.append(f"{name}: the mutated provider (play claims a voice for an invalid sound) "
                        f"was not caught by the in-player suite (exit {m.returncode}):\n{mout[-3000:]}")
    checks = len(re.findall(r"^\s+(ok  |FAIL)\s", out, re.M))
    return (f"audio: {checks} checks passed" if passed else "audio: FAILED") + \
           ("; mutation caught" if caught else "; mutation NOT caught")


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    print("$", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], cwd=cwd, capture_output=True, text=True)


def package(build: Path, work: Path, name: str, providers: dict | None,
            conformance: bool = False) -> tuple[Path, float, list[str]]:
    proj = work / name / "project"
    dist = work / name / "dist"
    shutil.rmtree(work / name, ignore_errors=True)
    r = run([build / f"engine_project{EXE}", "create", proj, "--name", f"shipped_{name}", "--template", "basic3d"])
    if r.returncode != 0:
        return dist, 0.0, [f"{name}: engine_project create failed:\n{r.stdout}{r.stderr}"]
    if providers is not None:
        pj = json.loads((proj / "project.json").read_text())
        pj["providers"] = providers
        (proj / "project.json").write_text(json.dumps(pj, indent=2))
    t0 = time.monotonic()
    r = run([build / f"engine_build{EXE}", proj, "--dist", dist, "--strict"]
            + (["--conformance"] if conformance else []))
    took = time.monotonic() - t0
    if r.returncode != 0:
        out = (r.stdout + r.stderr).splitlines()
        # The compiler's own lines first: a build failure's cause is usually far
        # above the last lines, which only say that the build failed.
        errs = [l for l in out if re.search(r"(error|Error|FAILED:|undefined reference|fatal)", l)][:40]
        tail = "\n".join(out[-25:])
        detail = ("\n".join(errs) + "\n  ...\n" if errs else "") + tail
        return dist, took, [f"{name}: engine_build failed (exit {r.returncode}):\n{detail}"]
    return dist, took, []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--build", type=Path, help="the development build directory")
    ap.add_argument("--work", type=Path, help="where to scaffold and package")
    ap.add_argument("--mutation", type=Path, help="a player with every provider: must FAIL the check")
    a = ap.parse_args()

    if a.mutation:
        lock = {"providers": [{"kind": "physics", "name": "jolt"}, {"kind": "scripting", "name": "lua"},
                              {"kind": "audio", "name": "none"}, {"kind": "renderer", "name": "none"}]}
        problems = check_symbols(a.mutation, lock)
        for p in problems:
            print("  found:", p)
        if any("did not choose" in p for p in problems):
            print("ok    the check fails on a player carrying providers its game did not choose")
            return 0
        print("FAIL  the check passed a player that carries every provider")
        return 1

    if not a.build or not a.work:
        ap.error("--build and --work are required")
    # Absolute: the players are started with their dist/ as the working
    # directory, where a relative path no longer names them (CI's first run).
    a.build, a.work = a.build.resolve(), a.work.resolve()
    a.work.mkdir(parents=True, exist_ok=True)
    failures: list[str] = []
    summary = ["| selection | engine_build | providers | symbols | run | conformance |", "|---|---|---|---|---|---|"]
    for name, providers in SELECTIONS.items():
        print(f"\n== {name} ==", flush=True)
        dist, took, errs = package(a.build, a.work, name, providers, name in WITH_CONFORMANCE)
        if errs:
            failures += errs
            summary.append(f"| {name} | failed after {took:.0f} s | | | | |")
            continue
        lock = json.loads((dist / "providers.lock.json").read_text())
        names = {p["kind"]: p["name"] for p in lock["providers"]}
        want = {"physics": "jolt", "scripting": "lua", "audio": "miniaudio", "renderer": "forward-bgfx"}
        want.update(providers or {})
        lock_ok = names == want
        if not lock_ok:
            failures.append(f"{name}: providers.lock.json names {names}, expected {want}")
        player = dist / f"engine_player{EXE}"
        # MSVC's symbols are in the link map beside the shipping tree's player,
        # which a game's dist/ never carries. Put it beside the packaged player
        # for the check only (this package's build is the tree's latest).
        ship_map = Path(__file__).resolve().parent.parent / "build-ship" / "engine_player.map"
        dist_map = player.with_suffix(".map")
        if os.name == "nt" and ship_map.exists():
            shutil.copyfile(ship_map, dist_map)
        sym = check_symbols(player, lock)
        if os.name == "nt" and dist_map.exists():
            dist_map.unlink()
        failures += [f"{name}: {p}" for p in sym]

        if name == "headless":
            r = run([player, dist, "--headless", "--frames", "120"], cwd=dist)
            ran = r.returncode == 0 and "120 frames headless" in r.stdout
            if not ran:
                failures.append(f"{name}: the packaged player did not run 120 frames headless "
                                f"(exit {r.returncode}):\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
            run_note = "120 frames" if ran else "FAILED"
        else:
            r = run([player, dist, "--headless", "--frames", "1"], cwd=dist)
            refused = r.returncode != 0 and "needs a display" in (r.stdout + r.stderr)
            if not refused:
                failures.append(f"{name}: with no display the packaged player should refuse its "
                                f"renderer by name (exit {r.returncode}):\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
            run_note = "refused with no display, by name" if refused else "FAILED"
        conf_note = conformance_runs(player, dist, name, lock, failures)
        summary.append(f"| {name} | {took:.0f} s | {'ok' if lock_ok else 'WRONG'} | "
                       f"{'ok' if not sym else 'WRONG'} | {run_note} | {conf_note} |")
        print(f"   {name}: engine_build {took:.0f} s, providers {names}", flush=True)

    print("\n" + "\n".join(summary))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a") as f:
            f.write("### Shipped player (WO-088)\n\n" + "\n".join(summary) + "\n\n")
    if failures:
        print("\nFAIL")
        for f in failures:
            print(" -", f)
        return 1
    print("\nok    both packaged players are what their games chose")
    return 0


if __name__ == "__main__":
    sys.exit(main())
