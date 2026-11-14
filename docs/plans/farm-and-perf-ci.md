---
status: plan
covers:
  - .github/workflows/perf.yml
  - tests/corebench/
---
# Measuring the engine: CI on every platform, and the home server for days

> **Status: plan, 2026-11-14.** The CI half is written (`perf.yml`, `tests/corebench/`)
> and has not run on GitHub yet. The home-server half is a plan. It builds on
> [`automated-testing-soak-fuzz-plan.md`](automated-testing-soak-fuzz-plan.md),
> which stays the source for the database schema (Part 5) and benchmark
> statistics (Part 7); this document says how to run it, with what exists now.

## 1. Two places, two jobs

| | GitHub CI (`perf.yml`) | The home server |
|---|---|---|
| Machines | Windows x64 and arm64, Linux x64 and arm64, macOS arm64, Android | one x86-64 Debian box, headless |
| Answers | does it work on every platform, and is the job system in range **there** | is this commit slower than last week's; does the engine survive three days |
| Noise | high: shared VMs, 2 to 4 cores, different hardware each run | low, once tuned (§4) |
| So it reports | medians and spread inside one job, never baselines | absolute numbers, against a baseline |
| Runs | on changes to core, weekly, on demand | every night, and a soak that never stops |

Neither replaces the other. A number from CI is never a baseline; a number from
the server is never assumed to hold on ARM or Windows.

## 2. The CI half (built)

- **`tests/corebench/`** is a standalone CMake project: the job system and memory
  manager from six engine sources plus TLSF, with no bgfx or Assimp. It builds
  in about a minute with any compiler, including the Android NDK, and produces
  `jobs_bench`, `jobs_fuzz_test` and `mem_test` for each backend.
- **`jobs_bench --json`** writes every measurement with its unit and the run's
  identity (backend, OS, CPU architecture, compiler, threads). Both CI and the
  server read this one format.
- **`perf.yml`**, per platform: fuzz (300 seeds), run `mem_test`, then benchmark
  five times. (Until 2026-11-14 it also ran enkiTS and reported a ratio; enkiTS
  was removed, WO-079.)
  A summary job turns all legs into one table (`tests/corebench/summarise.py`) on the
  run page.
- **Android:** arm64-v8a is built; x86_64 runs on an emulator (fuzz, `mem_test`,
  benchmark). GitHub has no arm64 Android host, so **the phone architecture is
  compiled, not run**. Running it needs a real device (§6).
- **Not in it yet:** the whole engine. `ci.yml` builds and tests that on the same
  five platforms. Engine-level benchmarks need the server first (§5), because
  they are too noisy on shared runners to mean anything.

## 3. How the server gets work: it pulls, nothing pushes to it

The repository is public. A GitHub **self-hosted runner** on the box would run
whatever workflow a pull request from anyone asks it to, on your home network.
GitHub's own guidance is not to use self-hosted runners with public
repositories. So:

- **The box pulls.** A systemd timer runs `git fetch` of `main` over anonymous
  HTTPS (the repository is public), builds what is new, runs the night's work,
  and writes results locally. No GitHub credentials on the box, nothing
  inbound, and a fork cannot reach it.
- **The box still does not push** (existing rule, §7.4 of the older plan).
  Findings become corpus candidates and a report a person commits.
- **You see results** in a static HTML report the box serves on the home
  network, and in the SQLite file, which `rsync` brings to the Mac.
- **Later, if wanted:** a commit-status line on GitHub through a fine-grained
  token that can set statuses and nothing else. Not in the first version.

## 4. Making the machine quiet (phase 0)

Nothing it measures is trustworthy before this. Once:
- `cpupower frequency-set -g performance`; turbo off (`intel_pstate/no_turbo` or
  `cpufreq/boost`), or left on and recorded.
- Kernel arguments `isolcpus=<bench cores> nohz_full=<same> rcu_nocbs=<same>`:
  half the cores for benchmarks, the rest for the OS, builds and fuzzing.
- Benchmarks pinned to the isolated cores with `taskset`; builds and fuzzing
  never run at the same time as a benchmark (one lock file).
- Swap off for benchmark runs; `kernel.perf_event_paranoid=1` so `perf` works
  without root; `systemd-coredump` on, so crashes keep their cores.
- Record the governor, turbo, kernel, compiler and CPU in every result (the
  `machine` row in Part 5). A change shows as a new machine, not a mystery step.

Then the **noise floor**: the same binary against itself, 7 runs, every workload.
Its median and spread are the smallest change the server is allowed to report.

## 5. What runs, every night

A nightly cycle, ordered so measurements happen on a quiet machine:

| Time | Lane | What |
|---|---|---|
| 01:00 | build | `main`'s new commit in four trees: Release, Release with profiling, ASan+UBSan, TSan |
| 01:40 | bench | §5.1, interleaved against the last baseline commit |
| 03:00 | profile | §5.2, on the same workloads |
| 03:30 | fuzz | §5.3 until morning, rotating sanitizer builds |
| always | soak | §5.4, one process, restarted only on a new night's build |

### 5.1 Benchmarks (absolute numbers, against a baseline)
- **Core:** `tests/corebench` (`jobs_bench`, and `mem_test` timings).
- **Engine, headless, through the C API** (`engCreateEngine` + `engStepWithDelta`,
  so the frame is the one the player runs):
  - the 2 000-box physics pile, ms per step;
  - N skinned characters animating, ms per frame;
  - extraction and culling of a 50 000-object scene. First check how much of it
    runs headless: `canPresent()` is false with no window, and whatever that
    skips is not measured;
  - scene load, cold and warm;
  - cook cold, warm and no-op (`engine_cook`), with numbers on record to anchor.
- **Method** (Part 7): A = baseline, B = new; interleave A and B; at least 7 runs;
  median and spread; flag only when the change is beyond the noise floor
  **and** statistically significant.
- **Instructions retired** (`perf stat`) beside every wall-clock number. It
  barely moves with noise, so it catches small regressions that timing cannot,
  as Rust's compiler benchmarks do. Cache misses and branch misses come with it.

### 5.2 Profiling
- `perf record -g` on each benchmark workload, folded into a flame graph per
  commit and kept, so "what got slower" is one click from "it got slower".
- The engine's own profiler channels (frame stats, memory per tag, job stats
  from `jobs::stats()`) written to the database instead of printed.

### 5.3 Fuzzing at scale
- `jobs_fuzz_test` with fresh seeds (the unit lane runs 60, CI 300; here,
  millions), under ASan and TSan in turn.
- The `fuzz-explore` targets, and the scene deserializer when it exists.
- A failure is deduplicated by signature, minimised, and written as a corpus
  candidate with its seed (Part 5.3).

### 5.4 The soak
One headless engine process for days, through the C API, on a scene built to
churn: entities spawned and destroyed, scenes loaded and unloaded, physics,
animation, null audio, and heavy job use. Every minute it records memory per tag,
mapped bytes, frame-time percentiles, thread and file-descriptor counts, and job
stats. It stops and keeps the evidence on:
- a crash or sanitizer report (core dump kept);
- a hang: no completed step within a watchdog limit;
- a slope: memory or descriptors growing steadily over hours, which is a leak.

### 5.5 Determinism
The same scene stepped on the server (Linux x86-64) and the Mac (arm64) must
give the same world hash each tick (`eng_step_test` already computes it). The
server runs its side nightly and keeps the hashes; the Mac compares when run.
This is the one lane that needs both machines, and the one that lockstep
multiplayer depends on.

## 6. What none of this measures

- **GPU and frame pacing.** The server is headless: nothing is drawn or
  presented. That stays on the Mac, and later a Windows box with a GPU.
- **Phones.** The emulator runs x86-64 Android. ARM phone numbers need a device
  on USB, either at the Mac or on the server running `adb` as part of the
  nightly. A cheap arm64 phone would do.
- **Thermal behaviour** of laptops and phones.

## 7. Order of work

Each step is useful on its own.
1. **CI half** green on all six legs (WO-080). Fix what the first run finds.
2. **Phase 0 on the box** (WO-081): the pull script, a systemd timer, four
   build trees, machine tuning, and the noise floor for `tests/corebench`.
3. **Results database and report** (WO-082): Part 5's schema, an ingester for
   `jobs_bench --json`, interleaved A/B with the statistics, the HTML report.
4. **Engine benchmarks and profiling** (WO-083): the headless workloads through
   the C API, `perf stat` and flame graphs.
5. **Soak, fuzz at scale, determinism** (WO-084).
