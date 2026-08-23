---
status: reference
---
# The Bug Ledger

**Every bug this engine has had, and the test that stops it coming back.**

Append-only. Entries are never deleted and ids are never reused, because the
value of this file is that it accumulates — the same reason a B-Rep kernel keeps
the model that broke booleans in 1997. Anyone can write the algorithms; what
cannot be reproduced is thirty years of knowing which specific inputs break
them.

## The rule

> A bug is not fixed when the code changes. It is fixed when a test fails
> without the change and passes with it.

`engine_doctor.py check` enforces the half a script can enforce: **every entry
must name a `pinned-by` test that exists.** An orphaned proof is an error, not a
warning, because a ledger of unenforced good intentions reads as coverage that
is not there.

```bash
python3 scripts/engine_doctor.py bugs              # list, with a class histogram
python3 scripts/engine_doctor.py bugs --class threading
python3 scripts/engine_doctor.py check             # gate (runs in the docs lane)
```

## Why this exists next to `issues.md`

`issues.md` files are *narrative*: what was wrong with a subsystem, what the
options were, what was chosen. They are long and worth reading. This is an
*index*: one line per defect, queryable, and mechanically tied to a test.

They serve different questions. "Why is the renderer shaped like this?" →
`issues.md`. "Which bugs have no regression test?" → here.

## Fields

| field | meaning |
|---|---|
| `found` | date the defect was identified |
| `class` | see below — decides the shape of the pinning test |
| `where` | the file the defect lived in |
| `symptom` | what was observed |
| `cause` | the actual mechanism, not the theory |
| `pinned-by` | **the test that fails without the fix** — checked to exist |
| `lane` | which lane surfaces it (`unit`, `asan`, `tsan`, `fuzz-regress`, …) |
| `proof` | how the regression test was verified to catch it |

### Classes, and the test each one implies

The class is not bookkeeping — it answers *"what kind of test do I write?"*
without leaving it to judgement each time.

| class | typical defect | pinned by |
|---|---|---|
| `memory` | out-of-bounds, use-after-free, leak | ASan/UBSan lane + a targeted test |
| `threading` | race, deadlock, livelock | TSan lane + stress |
| `abi` | layout, versioning, symbol contract | static asserts + an **offset** test |
| `parse` | malformed or hostile input | a corpus seed |
| `numeric` | determinism, precision, overflow | golden-value test |
| `perf` | regression in time or memory | perf test with a threshold |
| `build` | link graph, target wiring, packaging | assert on the built artifact |
| `logic` | plain wrong behaviour | ordinary unit test |
| `coverage` | a check that silently never ran | make it run, then assert it |

`parse` bugs already have a home that predates this file:
`tests/fuzz/corpus/*/regressions.seeds`, 65 seeds and counting. Those are not
duplicated here — the seed *is* the entry. This ledger exists because the other
eight classes had nowhere to go.

---

## BUG-0001 — UndoStack::push computed an iterator before begin()
- found:     2026-08-23
- class:     memory
- where:     src/editor/undo_stack.h
- symptom:   heap-buffer-overflow, 8-byte read before the deque's block map
- cause:     `begin() + m_index + 1` parses as `(begin() + m_index) + 1`, and m_index is -1 once everything has been undone. A deque iterator computes eagerly, so `operator+=` walked the block-pointer map on the spot. The final value was correct, which is why it never produced a wrong answer.
- pinned-by: tests/editor_undo_test.cpp
- lane:      asan
- proof:     the existing test already exercised the path and had been passing for weeks; it fails under ASan without the parentheses and passes with them.

## BUG-0002 — base64 helper read past its buffer on the short final group
- found:     2026-08-23
- class:     memory
- where:     tests/cooker_test.cpp
- symptom:   stack-buffer-overflow, plus a UBSan "index 80 out of bounds for type 'unsigned char[80]'"
- cause:     80 % 3 == 2, so the last base64 group has two bytes, but the loop read `buf[i+1]` and `buf[i+2]` unconditionally. It also encoded a character derived from stack garbage, and round-tripped only because the decoder discards that byte as padding.
- pinned-by: tests/cooker_test.cpp
- lane:      asan
- proof:     ASan and UBSan both report at the exact line without the bounds check.

## BUG-0003 — every external thread shared enkiTS thread-slot 0
- found:     2026-08-23
- class:     threading
- where:     src/runtime/jobs/jobs_enkits.cpp
- symptom:   livelock — workers spin forever in enkiTS's lock-free pipe while the caller blocks in WaitforTask. Reproduced ~1 run in 5 under TSan; stuck at 180s, so starvation rather than slowness.
- cause:     enkiTS indexes per-thread state by thread number and returns 0 for "the thread that initialised the scheduler AND all unregistered threads". The engine never called RegisterExternalTaskThread and left numExternalTaskThreads at 0, so any kit or provider thread calling jobs::parallelFor drove slot 0 concurrently with the main thread.
- pinned-by: tests/api_primitives_test.cpp
- lane:      tsan
- proof:     mutation — restored the pre-fix behaviour (`ensureThreadRegistered` returning true unconditionally) and the hang reproduced on run 2 of 12; reverted and 12/12 completed.
- note:      this was live on a path published two commits earlier — the audio provider ABI tells providers to send decode, streaming and propagation work to `services->parallelFor` from their own workers.

## BUG-0004 — the audio ABI's frozen-layout asserts had never been compiled
- found:     2026-08-22
- class:     coverage
- where:     include/engine/engine_audio_provider.h
- symptom:   none observable. Every static assert in the header was dead.
- cause:     the header carried ENGINE_AUDIO_FROZEN assertions and a comment claiming the C and C++ builds checked them, but NO translation unit in the tree included it. The Rust suite pinned only its own transcription, so the C structs could have been reshaped freely with every test still green.
- pinned-by: tests/audio_abi_check.c
- lane:      unit
- proof:     swapped two uint64 fields in EngineAudioStats — the build SUCCEEDED with every static assert blind to it, and only the new offset check caught it.

## BUG-0005 — the API table's version gate rejected every older module
- found:     2026-08-19
- class:     abi
- where:     include/engine/engine_api_client.h
- symptom:   appending three API groups invalidated all three Kits and the game module wholesale — not degraded, rejected.
- cause:     the client shim gated on `structSize != sizeof(...)` and `have == want`. The first rejects a module because the table got BIGGER; the second means improving a group to v2 locks out every module asking for v1.
- pinned-by: tests/api_abi_compat_test.cpp
- lane:      unit
- proof:     the test lays an older, shorter table definition over the live table and asserts the function pointers resolve identically, and asserts the one direction that must keep failing (older host, newer module).

## BUG-0006 — warned[7] guarded nine API groups
- found:     2026-08-19
- class:     memory
- where:     include/engine/engine_api_client.h
- symptom:   out-of-bounds write on every warning past the seventh group.
- cause:     the array size was a literal typed once; the group enum grew three times after it.
- pinned-by: tests/api_abi_compat_test.cpp
- lane:      unit
- proof:     found while fixing BUG-0005; the array is now sized from the enum.

## BUG-0007 — sample rate and host clock could not be correlated
- found:     2026-08-22
- class:     abi
- where:     include/engine/engine_audio_provider.h
- symptom:   `startSampleTime` was decorative — the engine could not compute a future sample instant.
- cause:     `EngineAudioStats` reported `samplesPlayed` with no host timestamp. A count is not a mapping: the game thread learned how many samples had played but not WHEN that was true, and an arbitrary amount of time passed between publish and read.
- pinned-by: tests/audio_conformance/src/lib.rs
- lane:      unit
- proof:     the suite checks the two clocks agree on elapsed time, which is what proves they were sampled together rather than merely both present.

## BUG-0008 — miniaudio's node-graph clock reported as a device clock
- found:     2026-08-22
- class:     logic
- where:     src/audio/miniaudio_provider.cpp
- symptom:   `samplesPlayed` frozen at 0. The conformance suite reported "0.0 ms of samples vs 125.2 ms of host".
- cause:     letting `ma_engine` create its own device meant reading `ma_engine_get_time_in_pcm_frames`, which is the NODE GRAPH's time and only advances while something is mixing. With nothing playing it does not move at all.
- pinned-by: tests/audio_conformance/src/lib.rs
- lane:      unit
- proof:     caught by BUG-0007's assertion one commit after that assertion was written. Fixed by having the provider own the ma_device and count frames in its own callback.

## BUG-0009 — GLFW was linked into every build, including SDL3 ones
- found:     2026-08-23
- class:     build
- where:     src/runtime/input/input_system.h
- symptom:   an SDL3 build linked GLFW; a host supplying its own window dragged in a windowing library it had no use for.
- cause:     input_system.h included `<GLFW/glfw3.h>` and branched on the backend in six places. Because input.h, input_map.h, input_sources.h, runtime.cpp and the editor all include it, one include decided the link line for the whole engine.
- pinned-by: src/CMakeLists.txt
- lane:      unit
- proof:     stashed the change and reconfigured — `libglfw3.a` is in the SDL3 `engine_player` link line before and absent after. NOT YET A TEST: this is verified by hand, and BUG-0009 is the reason the `build` class exists. See the open item below.

## BUG-0010 — kit_lifecycle_test could never pass in CI
- found:     2026-08-23
- class:     coverage
- where:     tests/CMakeLists.txt
- symptom:   a permanently-failing test in the `unit` lane.
- cause:     the test registered whenever `fps_shooter/project.json` existed — which is committed — but its actual fixture is a kit module built from a separate gitignored repo. It never mattered because CI ran only the docs lane, and would have made the first gating lane worthless.
- pinned-by: tests/CMakeLists.txt
- lane:      unit
- proof:     the guard now requires the kit module itself; configure prints a STATUS line naming what is missing, and all three lanes report 100%.

## BUG-0011 — two material representations, and which one ran was incidental
- found:     2026-08-23
- class:     logic
- where:     src/render/material.h
- symptom:   none visible. A surface rendered through one of two upload paths depending on where its material came from, and there was no way to tell which short of reading the branch.
- cause:     Material carried dedicated fields (baseColorFactor / roughness / metallic / textures) for mesh-embedded materials AND uniform blocks for cooked .cmat assets, with ForwardPipeline branching on a `dataDriven` flag. Two sources of truth for the same concept, able to disagree.
- pinned-by: tests/material_form_test.cpp
- lane:      unit
- proof:     mutation — four ways of breaking the synthesis (roughness into the wrong component, the normal sampler undeclared when untextured, a shader name that would disable instancing, accessors returning copies instead of a window) each failed their own assertion and passed on revert.
- note:      not a defect that had bitten yet; recorded because "either could be the one that runs" is the shape of a bug rather than a bug, and the ledger is where that belongs.

## BUG-0012 — glTF's absent-factor default reaches a renderer that cannot use it
- found:     2026-08-23
- class:     logic
- where:     src/assets/cookers/mesh/mesh_cooker.cpp
- symptom:   fps_shooter's pistol renders fully metallic and fully rough. Inspector reads Roughness 1.00 / Metallic 1.00.
- cause:     pistol_without_mag.gltf omits roughnessFactor and metallicFactor. glTF defines an absent factor as 1.0, so cgltf reports 1.0/1.0 and the cooker records it faithfully. The defect is downstream: glTF's factors MULTIPLY a metallicRoughness texture, and the forward pipeline samples only baseColor and normal — with nothing to multiply, 1.0/1.0 is an unapplied coefficient rather than an authored look. Not a cook error; a renderer gap.
- pinned-by: tests/cooked_format/tests/gltf_pbr_factors.rs
- lane:      unit
- proof:     mutation — clamping the cooker to 0.7/0.0 fails the assertion; reverted and it passes. The test drives the real cooker through engine_cook_worker's CLI and parses the .cooked bytes from outside C++.
- note:      FIRST DIAGNOSIS WAS WRONG and is recorded because the correction is the useful part. It blamed a gltf_importer.cpp change made during the Phase 5 migration. Reading the cooked file settled it: the .cooked already held 1.0/1.0, and the scene loads the pistol via cookedPath, a path the direct importer never touches. The real fix is sampling MR/ARM textures — a rendering feature. Until then the value is pinned so it is deliberate.

## BUG-0014 — a behaviour change smuggled into a behaviour-preserving migration
- found:     2026-08-23
- class:     logic
- where:     src/assets/importers/gltf_importer.cpp
- symptom:   none observed — the affected path (direct source import) is not used by fps_shooter's scene, which loads cooked meshes.
- cause:     MINE. While migrating materials to blocks I also changed the direct glTF importer to pass pbr.roughness_factor / pbr.metallic_factor, which it had never read. Presented as a free improvement inside a change whose whole requirement was preserving behaviour, and it would have shifted every directly-imported glTF from 0.7/0.0 to glTF's absent-default 1.0/1.0.
- pinned-by: src/assets/info.md
- lane:      unit
- proof:     reverted to Material::kStdDefaultRoughness / kStdDefaultMetallic. The defaults became NAMED CONSTANTS in the same change: three import paths had been relying on them implicitly by never assigning the members, and an implicit default is invisible at the call site — which is how it survived review.
- note:      no automated test. The importer path has no cooked artifact to parse and no process boundary, so pinning it needs an importer test with a glTF fixture. On the open list.

## BUG-0013 — the source-import upload path drops cooked roughness/metallic
- found:     2026-08-23
- class:     logic
- where:     src/runtime/services/async_loader/upload.cpp
- symptom:   a source-imported mesh shades with 0.7 / 0.0 regardless of what its material says.
- cause:     the path memcpy's baseColorFactor and nothing else, so the roughness and metallic values MaterialGPUData carries are never applied. Predates the migration.
- pinned-by: docs/process/bug-ledger.md
- lane:      unit
- proof:     NOT FIXED. Found while auditing what the migration changed — applying the carried values would have altered shading on every source-imported mesh, which a behaviour-preserving migration must not do. Recorded rather than fixed in passing; it needs its own before/after on real content.

---

## Open — recorded so they are not forgotten

These are known, unpinned, and deliberately visible rather than tidied away.

- **BUG-0009 has no automated test.** The check is "assert on the built
  artifact": read the link line, or `nm` the binary, the way the
  `shipping-runtime` CI lane already asserts Assimp is absent. Until that
  exists, nothing stops GLFW creeping back into an SDL3 build.
- **CI has never run the unit lane.** It builds on three platforms and runs only
  the docs contract. The sanitizer job added alongside this ledger is the first
  lane in the project's history to execute `ctest -L unit` on a push — which
  also means the 63 test binaries have only ever been run on developer
  machines.
- **Cooked formats have an independent reader now — every one of them.**
  `tests/cooked_format/` parses `.cooked`, `.cmat`, `.ctex`, `.cshader` and the
  DDC record manifest from outside C++, with no engine code. A C++ save/load
  round-trip proves the writer and reader agree WITH EACH OTHER and nothing
  more; both can move together and stay green.

  What remains open is the second half of the same idea: the readers pin
  STRUCTURE, not content equality across platforms. Diffing two machines' cooked
  bytes over the same corpus is the DDC's load-bearing assumption (assetlib
  issues.md O2) and the cross-ISA determinism lane in the soak plan — and this
  crate is now the tool that could do it without either engine running.
- **Four of those readers had never once run outside ctest, and said `ok`.**
  Every test in `tests/cooked_format/` that COOKS a real asset — `.cmat`,
  `.ctex`, `.cshader`, the glTF factor case — began with a `println!` and an
  early `return` when `ENGINE_BUILD_DIR` was unset. `cargo test --quiet`, which
  is how CMake invokes the crate, captures stdout: a lane that had silently
  stopped exercising anything reported exactly what a passing lane reports.
  Running the crate by hand showed `6 passed` while the shader test did nothing
  at all. The eight tests that DID run all build structs by hand, so the entire
  "read what the engine actually wrote" premise rested on the four that were
  invisible.

  Fixed by `ENGINE_REQUIRE_COOK_TESTS=1`, which the CMake entry sets and which
  turns a skip into a failure — a bare `cargo test` still skips, because there
  no build directory was ever promised. Same shape as `ENGINE_AUDIO_FROZEN`
  asserting in a header no translation unit included: the check existed, was
  correct, and had never executed.

- **The independent readers repaired corrupt text instead of refusing it.**
  All four byte-to-text sites used `String::from_utf8_lossy`, which substitutes
  U+FFFD per bad byte and returns a string that parses cleanly, so a corrupt
  field arrived looking like a slightly odd field.

  In the DDC manifest that was not cosmetic. The repair ran BEFORE
  `is_plain_filename`, so the check deciding whether a member name can escape
  its directory was inspecting text the engine never wrote, while the C++
  validates the raw bytes. Demonstrated by reverting the fix: a name containing
  a raw `0xFF` came back `Ok` as `"bad\u{FFFD}name.ctex"` — the repair turning
  an unexaminable byte into a benign character and walking it past the check.
  U+FFFD is three bytes, none of them a separator or a control character, so
  lossy decoding could only ever make a name look SAFER than it was.

  All four now refuse. The manifest reader is deliberately stricter than
  `ddcFetchRecord`, which accepts a non-UTF-8 member name because a POSIX
  filename is an arbitrary byte string: sibling names are generated by the
  cookers (`<uuid>_t0.ctex`) and never carried from a source filename, so no
  manifest the engine writes can trip it, while one arriving from a shared store
  with such a name in it is worth stopping for. All three sites are
  mutation-verified.

- **The backfill is incomplete.** Twelve `issues.md` files hold roughly 2,400
  lines of documented, already-fixed defects. The entries above are today's
  findings plus the recent ABI and audio work. Older subsystems — the renderer's
  R10–R21, the asset cookers, assetlib — are not yet indexed, and until they are
  the histogram under-reports.
