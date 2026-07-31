---
status: unreviewed
---
# Game Engine Compiler Test Sheet (Mon Jul 6)

A practical checklist for using your compiler and toolchain to catch bugs before they hit a build. Organized so you can work through it top to bottom, or use it as a CI setup reference.

---

## 1. Warning Flags (do this first — it's free)

Enable in every build config. Treat as errors (`-Werror` / `/WX`) in CI at minimum.

### GCC / Clang
| Flag | Catches |
|---|---|
| `-Wall -Wextra -Wpedantic` | Baseline broad coverage |
| `-Wshadow` | Variable shadowing (classic footgun) |
| `-Wconversion` | Implicit narrowing / float↔int conversions |
| `-Wfloat-equal` | Comparing floats with `==` |
| `-Wcast-align` | Misaligned pointer casts (UB, crashes on ARM/consoles) |
| `-Wnon-virtual-dtor` | Missing virtual destructor on polymorphic base |
| `-Wold-style-cast` | C-style casts hiding unsafe reinterpret_cast |
| `-Wuseless-cast` | Redundant casts (GCC only) |
| `-Wdouble-promotion` | Silent float→double promotion in hot paths |
| `-Wnull-dereference` | Likely null deref paths |

### MSVC
| Flag | Catches |
|---|---|
| `/W4` | Baseline broad coverage |
| `/Wall` | Very aggressive (noisy — expect to suppress some) |
| `/w14640` | Thread-unsafe static init |
| `/analyze` | See static analysis section below |

**Checklist:**
- [ ] Warnings enabled at max reasonable level on all three toolchains you target
- [ ] Warnings-as-errors enabled in CI (not necessarily local dev builds)
- [ ] `-Wconversion` and `-Wfloat-equal` specifically enabled (float bug prevention)

---

## 2. Static Analysis (compiler-integrated)

Deeper than warnings — cross-function flow analysis.

- [ ] **Clang Static Analyzer** (`scan-build`) — run periodically, not necessarily every commit (slow)
- [ ] **GCC `-fanalyzer`** (GCC 10+) — same category, if GCC is in your toolchain
- [ ] **MSVC `/analyze`** — enable in a dedicated CI/nightly build
- [ ] **clang-tidy** — run on every PR with at minimum:
  - `bugprone-*`
  - `performance-*`
  - `modernize-*` (optional, more style than bugs)
  - `cert-*` (if you care about security-adjacent rules)

---

## 3. Sanitizers (runtime, compiler-instrumented)

Compile special debug/CI builds with these. Don't ship them, but run your smoke tests and stress scenes through them regularly.

| Sanitizer | Flag | Catches | Cost |
|---|---|---|---|
| AddressSanitizer | `-fsanitize=address` | Buffer overflows, use-after-free, use-after-scope, double-free | ~2x slowdown |
| UndefinedBehaviorSanitizer | `-fsanitize=undefined` | Signed overflow, misaligned access, null pointer arithmetic, bad enum values | Low |
| ThreadSanitizer | `-fsanitize=thread` | Data races (job systems, async loading, parallel physics) | ~5-15x slowdown, run separately |
| MemorySanitizer | `-fsanitize=memory` | Use of uninitialized memory | Clang/Linux only |

**Checklist:**
- [ ] CI lane: build with `-fsanitize=address,undefined`, run smoke test + stress scene
- [ ] Separate CI lane: build with `-fsanitize=thread` if engine has any multithreading (can't combine with ASan)
- [ ] Nightly/weekly: run full regression suite under ASan+UBSan (slower, so not every commit)
- [ ] MSan lane if you have Linux+Clang available (optional, high value if feasible)

---

## 4. Optimization / Vectorization Reports

Directly relevant to hot loops (particle systems, skinning, physics broad-phase, culling).

### Clang
```
-Rpass=loop-vectorize            # what got vectorized
-Rpass-missed=loop-vectorize      # what didn't, and why
-Rpass-analysis=loop-vectorize    # detailed reasoning
```

### GCC
```
-fopt-info-vec-optimized          # vectorized loops
-fopt-info-vec-missed             # missed loops + reason
-fopt-info-inline                 # inlining decisions
```

### MSVC
```
/Qvec-report:2                    # vectorization report
```

**Checklist:**
- [ ] Run `-Rpass-missed`/`-fopt-info-vec-missed` on known hot loops quarterly or after major refactors
- [ ] Confirm no unexpected scalar fallback in entity update / physics / render culling loops
- [ ] Investigate any "unsafe dependent memory operations" or aliasing-related missed-vectorization messages

---

## 5. Floating-Point Determinism Flags

Relevant if you need lockstep multiplayer, replays, or save-state/rewind.

- [ ] Decide explicitly: strict FP (`/fp:strict` MSVC, avoid `-ffast-math`) vs fast FP, per subsystem
- [ ] Pin FMA/AVX flags project-wide (`-mfma`, `-mavx`) so all binaries agree on what's allowed — don't let this vary silently between debug/release or between machines
- [ ] Never use `-ffast-math` / `/fp:fast` on any code path required to be deterministic
- [ ] Audit whether any library (audio, math) is changing global FPU state (FTZ/DAZ) at startup

---

## 6. Assembly / IR Inspection

For when you need to see exactly what got generated.

- [ ] Use **Compiler Explorer** (godbolt.org) to compare GCC/Clang/MSVC output for suspicious hot functions
- [ ] `-S` (GCC/Clang) or `/FA` (MSVC) to dump assembly locally
- [ ] `-emit-llvm` (Clang) to inspect IR before final codegen, if debugging optimization pass behavior

---

## 7. Profile-Guided Optimization (as diagnostic, not just perf)

- [ ] Run PGO instrumentation build (`-fprofile-generate` / `/GENPROFILE`) during an automated bot playthrough or stress test
- [ ] Review the profile data for real hot functions / branch frequencies (often surprising vs assumptions)
- [ ] Bake resulting profile into `-fprofile-use` / `/USEPROFILE` shipping build once stable

---

## 8. Build-Time Diagnostics

Useful if compile times are a problem (common with templated ECS/math libs).

- [ ] Run `-ftime-trace` (Clang) periodically, inspect in Chrome's trace viewer, find the worst-offending headers/templates

---

## 9. CI Lane Summary (recommended minimum setup)

| Lane | Trigger | Build config |
|---|---|---|
| Fast unit + smoke | Every commit | Warnings-as-errors, normal debug build |
| Static analysis (clang-tidy) | Every PR | Standard build + clang-tidy pass |
| ASan + UBSan | Every PR or nightly | `-fsanitize=address,undefined`, run smoke + stress scene |
| TSan | Nightly | `-fsanitize=thread`, run multithreaded stress scene |
| Full regression + sanitizers | Nightly/weekly | Full suite under ASan+UBSan |
| Vectorization report review | Quarterly / after refactors | `-Rpass-missed` on hot loops |
| PGO profile refresh | Before major releases | Instrumented build + bot playthrough |

---

## Quick-start priority order (if starting from zero)

1. Turn on `-Wall -Wextra -Wconversion -Wshadow` (or MSVC equivalents), fix what surfaces
2. Add clang-tidy to PRs with `bugprone-*` and `performance-*`
3. Add an ASan+UBSan CI lane running your smoke test + a stress scene
4. Add a TSan lane if you have any threading at all
5. Pull vectorization-missed reports on your 3-5 hottest loops
6. Decide your FP-strictness policy per subsystem before you build any networking/replay features on top of it
