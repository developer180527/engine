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

## BUG-0015 — a bounds check that overflowed and accepted the worst input
- found:     2026-08-24
- class:     memory
- where:     tests/cooked_format/src/shader.rs
- symptom:   at `offset = u32::MAX` a debug build panicked ("attempt to add with overflow") and a RELEASE build wrapped to a small number and returned Ok — the bounds check accepting the most out-of-bounds value representable, which is exactly the input it exists to reject.
- cause:     `params_fit_their_uniforms` computed `offset + components` in u32 on an offset read RAW FROM THE FILE. Its sibling `variant_ranges_in_bounds`, two functions above in the same file, had already been widened to u64 — nothing held the pair together, so they drifted apart in a single sitting.
- pinned-by: tests/cooked_format/tests/cshader_ddc.rs
- lane:      unit
- proof:     mutation in BOTH profiles — debug reports "attempt to add with overflow" at shader.rs:177, release fails the assertion with "the addition wrapped and the check accepted the worst possible input". The test pins BOTH functions plus the in-bounds boundaries either side, because the defect is the drift between siblings rather than the arithmetic in one of them.

## BUG-0016 — four cooked-asset lanes that never ran and reported ok
- found:     2026-08-24
- class:     coverage
- where:     tests/cooked_format/
- symptom:   none. `cargo test --quiet` reported passing lanes that had silently stopped exercising anything.
- cause:     every test that COOKS a real asset began with a `println!` and an early return when ENGINE_BUILD_DIR was unset. CMake invokes the crate with `--quiet`, WHICH CAPTURES STDOUT — so the reason went into a buffer nobody reads and the lane reported precisely what a passing lane reports. Four tests, not one: cmat, ctex, cshader and the glTF factor case — every test that touches real cooked bytes. The eight that did run all build structs by hand, so the crate's entire premise rested on the four that were invisible.
- pinned-by: tests/cooked_format/src/harness.rs
- lane:      unit
- proof:     ENGINE_REQUIRE_COOK_TESTS=1 (set by the CMake entry) turns a skip into a FAILURE; verified by running the crate with the flag and no build dir, which fails rather than reporting ok. A bare `cargo test` still skips, because there the developer knows there is no engine to point at.
- note:      the same shape as BUG-0004 three weeks later — a check that exists, is believed to run, and does not. I had verified two of these four by hand with `--nocapture` and then did not make the check mechanical, which is the failure this ledger exists to stop.

## BUG-0017 — assertions that disarmed instead of failing
- found:     2026-08-24
- class:     coverage
- where:     tests/cooked_format/tests/cshader_ddc.rs
- symptom:   none observable — the guard would simply stop guarding.
- cause:     the four checks pinning standard.shader's parameter offsets against Material::kStd* were written as `if let Some(p) = sh.param("roughness") { assert_eq!(p.offset, 1) }`, which pins the offset when the param is there and passes SILENTLY when it is not. Renaming or dropping `roughness` removed the guard rather than tripping it.
- pinned-by: tests/cooked_format/tests/cshader_ddc.rs
- lane:      unit
- proof:     presence is asserted now, with a message naming the constant and the file to move it in. This was the self-agreeing-check pattern the whole crate exists to replace, reintroduced inside it.

## BUG-0018 — lossy UTF-8 repair ran BEFORE the name-safety check
- found:     2026-08-24
- class:     logic
- where:     tests/cooked_format/src/ddc_manifest.rs
- symptom:   a DDC manifest member name containing a raw 0xFF byte was ACCEPTED: `Ok(Manifest { members: [..., name: "bad\u{FFFD}name.ctex"] })`.
- cause:     `read_manifest_bytes` was `read_manifest(&String::from_utf8_lossy(bytes))`, so the repair ran before `is_plain_filename` — the check deciding whether a member name can escape its directory. U+FFFD is three bytes, none a separator and none a control character, so lossy decoding can only ever make a name look SAFER than the bytes actually are. The C++ validates the raw bytes (`for (unsigned char c : name)`), so the two sides were not inspecting the same string at all.
- pinned-by: tests/cooked_format/tests/cshader_ddc.rs
- lane:      unit
- proof:     mutation — restoring the lossy decode returns Ok with the U+FFFD name. Covered across three distinct paths (the manifest, a .cshader length-prefixed string, the mesh's NUL-padded fixed-width texture path), each with a clean-input control so the rule is not a blanket refusal.
- note:      the reader is now deliberately STRICTER than ddcFetchRecord, which accepts a non-UTF-8 member name because a POSIX filename is an arbitrary byte string. Chosen knowingly: sibling names are generated by the cookers (<uuid>_t0.ctex) and never carried from a source filename, so no manifest the engine writes can trip it, while one arriving from a shared store with such a name is worth stopping for. Byte-exact parity would mean Vec<u8> member names and an API change.

## BUG-0019 — the reference audio provider's clock could not keep up with itself
- found:     2026-08-24
- class:     threading
- where:     tests/audio_conformance/src/reference.rs
- symptom:   the macOS CI leg — the GATING one — failed for several commits, taking two tests with it, while passing 15/15 on a 12-core laptop.
- cause:     the stand-in driver thread was `sleep(period); samples += frames`, so its effective sample rate is `frames / how long that sleep ACTUALLY took` — and sleep guarantees only a LOWER bound. On a 3-core runner with several tests in parallel the clock advanced at under half real time, and the suite's own "the sample clock and the host clock agree within 0.5x-2.0x" check failed. The assertion was right and the reference was wrong.
- pinned-by: tests/audio_conformance/src/reference.rs
- lane:      unit
- proof:     reproduced deterministically by injecting a 3x sleep overshoot — identical failure, identical message ("101.3 ms of samples vs 361.4 ms of host"); the fix passes 3/3 under the same injection. samplesPlayed now derives from Instant::elapsed(), which is what real hardware does: the device clock runs whether or not the callback thread was scheduled promptly — that is what an underrun IS.
- note:      the first attempt is worth recording. Polling until the clock moved AT ALL traded a starvation failure for a QUANTISATION one (2.7 ms of samples vs 6.3 ms of host): a provider advances in buffer-sized chunks, so over a one-buffer window the quantisation IS the measurement. The window has to be long enough to make a buffer small.

## BUG-0020 — a process-global counter driven by two parallel tests
- found:     2026-08-24
- class:     threading
- where:     tests/audio_conformance/src/reference.rs
- symptom:   `reference_stride_is_walked_by_stride` could read the other reference test's value and report a stride bug that never happened.
- cause:     LAST_WELL_FORMED is process-global and both reference tests drive it; cargo runs tests on parallel threads by default.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      unit
- proof:     serialised behind REFERENCE_LOCK, so the counter is read by the test that wrote it.

## BUG-0021 — std::memcpy without <cstring>, in four files
- found:     2026-08-24
- class:     build
- where:     src/
- symptom:   the Linux CI legs failed to compile. macOS never noticed.
- cause:     libc++ pulls <cstring> in transitively; libstdc++ does not.
- pinned-by: .github/workflows/ci.yml
- lane:      unit
- proof:     found by GREPPING for the pattern rather than by waiting for CI. The build stops at the first error, so CI could only ever report one of the four — three more red runs to find them one at a time. Re-verified after the fix: zero files under src/ or modules/ use std::memcpy/memset/memcmp/strlen/strcmp without the include.

## BUG-0022 — maRealloc read past the end of every block it grew
- found:     2026-08-25
- class:     memory
- where:     src/audio/miniaudio_provider.cpp
- symptom:   SIGSEGV on Linux inside ma_decoder__full_decode_and_uninit, i.e. on every sound decode. Nothing ever went wrong on macOS.
- cause:     the host services interface has no realloc — deliberately — so the shim did alloc + copy + free and bounded the copy by the NEW size. Growing 64 KB -> 128 KB therefore read 64 KB past the end of the old block. On macOS the over-read landed inside the tagged heap's mapped region and was invisible.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      unit
- proof:     reproduced under Linux in a container with the provider built standalone (it is one self-contained TU by design, so no engine build is needed); gdb pointed straight at maRealloc. Mutation-verified — restoring the old bound brings the segfault straight back. Blocks now carry their size in a 16-byte alignas(16) prefix, static-asserted so the payload stays SIMD-aligned, and the copy is bounded by the MINIMUM, the only bound correct in both directions.
- note:      MINE, and the comment is the worst part: it said the copy was "safe only while miniaudio uses this path to GROW", which is exactly inverted — growing is the unsafe direction. I identified the hazard and drew the opposite conclusion from it, which is worse than not considering it, because nothing executable disagreed with a comment that read as reasoned.

## BUG-0023 — the one test that exercises the audio provider is the one ASan cannot see
- found:     2026-08-25
- class:     coverage
- where:     tests/CMakeLists.txt
- symptom:   BUG-0022 was a textbook heap over-read that ASan would have caught instantly, on a lane that runs on every push, and it survived to a Linux SIGSEGV.
- cause:     audio_abi_conformance is EXCLUDED from sanitizer builds, because the Rust harness dlopens an instrumented module and ASan aborts with "interceptors are not working". The exclusion is correct in itself; its consequence was not noticed — the provider's only behavioural coverage lives in the excluded test, so that whole TU is outside the sanitizer's reach.
- pinned-by: docs/process/bug-ledger.md
- lane:      unit
- proof:     NOT FIXED. Recorded rather than patched: the options (instrument the Rust side with nightly's -Zsanitizer, or add a C++ harness that links the provider directly) are both real work and the choice deserves its own pass. Until then the provider is the one engine TU with behavioural tests and no sanitizer coverage.

## BUG-0024 — engineInit published without a happens-before edge
- found:     2026-08-25
- class:     threading
- where:     src/audio/miniaudio_provider.cpp
- symptom:   TSan reported a race between maCreate and the CoreAudio callback thread.
- cause:     engineInit was a plain bool, written by the creating thread and read by the callback. The ORDERING in the source was already correct — set before ma_device_start — but source order is not a happens-before edge, and ma_device_init has already created the callback thread by that point, so nothing made the write visible.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      tsan
- proof:     a release store paired with a SINGLE acquire load at the top of the callback, which orders everything else maCreate published.

## BUG-0025 — a cross-thread field hidden under a comment saying it was not one
- found:     2026-08-25
- class:     threading
- where:     src/audio/miniaudio_provider.cpp
- symptom:   none reported by TSan; found by reading after it flagged BUG-0024.
- cause:     expectedPeriodNs sat under the comment "Callback-local, touched only by the audio thread" and is computed in maCreate. Grouping a cross-thread field beneath a comment ASSERTING it cannot be one is how it stayed invisible — a reader checking for races skips the block the comment excludes.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      tsan
- proof:     now an atomic, loaded relaxed in the callback. Worth noting TSan found one race and reading found the other: the tool bounds what it can observe in a single run, so a report is a starting point rather than a complete list.

## BUG-0026 — an overrun counter that measured the runner instead of the mixer
- found:     2026-08-25
- class:     logic
- where:     src/audio/miniaudio_provider.cpp
- symptom:   "NO callback overruns under a 2000-command burst" failed on the Linux CI legs, 9 overruns counted — an assertion about our mixer failing on a fact about the machine.
- cause:     a Linux runner has no sound card: ALSA prints "cannot find card '0'" and then returns a working 48 kHz device anyway, whose callback cadence is whatever the scheduler felt like. An overrun means "we missed a HARDWARE deadline"; with no hardware there is nothing to miss.
- pinned-by: tests/audio_conformance/tests/conformance.rs
- lane:      unit
- proof:     ENGINE_AUDIO_NO_HARDWARE=1 covers both shapes of "no dependable audio device", permits the null-backend fallback and stops the timing assertions. ctest sets it; a shipped game never does, so a real device failure stays visible as E_NO_DEVICE. The FIRST version of the flag meant only "we fell back to the null backend" and could never trigger, because ALSA had already succeeded.

## BUG-0027 — a generated file gated on content the commit itself changes
- found:     2026-08-25
- class:     coverage
- where:     scripts/engine_doctor.py
- symptom:   docs_status_current failed three runs on the gating leg while ENGINE_STATUS.md was, in every sense anyone cared about, current.
- cause:     ENGINE_STATUS.md carries git-DERIVED data — the per-subsystem "code last changed" column and the stale-doc count — so regenerating before a commit and after it differ by construction: working-tree edits are not in history yet, and the moment they are, every doc covering them moves. Stale docs read 7 before and 14 after, same tree, same script. Committing the file invalidated it.
- pinned-by: scripts/engine_doctor.py
- lane:      docs
- proof:     the gate now compares STRUCTURE — subsystems, tiers, docs, test counts — and normalises dates away; freshness stays with `engine_doctor check`, which computes it live and reports warnings. Verified by making a real commit and checking after it.
- note:      two comments already in that function recorded the same lesson from two other causes, which is the sign it needed the rule stated rather than patched a third time: A GENERATED FILE CANNOT BE GATED ON CONTENT THAT CHANGES BECAUSE OF THE COMMIT THAT CONTAINS IT.

## BUG-0028 — the replacement operator new promised less alignment than the standard requires
- found:     2026-08-26
- class:     memory
- where:     src/core/mem_counters.cpp
- symptom:   animator_system_test SEGFAULT on Windows x64 only. Linux x64, macOS arm64 and Windows arm64 all passed.
- cause:     the replacement `operator new(size_t)` requested `alignof(std::max_align_t)`, but the standard requires it to return storage aligned to `__STDCPP_DEFAULT_NEW_ALIGNMENT__`. Those differ: max_align_t is 8 on macOS arm64 AND MSVC x64, while the required new-alignment is 16 on both. libc++ and MSVC's STL route a type through the ALIGNED operator new only when `alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__`, so a 16-byte-aligned type is NOT over-aligned — it takes the plain overload and relies on the guarantee this replacement was breaking. Every `std::vector<ozz::math::SoaTransform>` therefore came back 8-byte aligned, and ozz writes those buffers with `_mm_store_ps`, which faults.
- pinned-by: tests/animator_system_test.cpp
- lane:      unit
- proof:     mutation — restoring `alignof(std::max_align_t)` reproduces "localsPrev is at 0x109c092d8, which is 8-byte aligned, not 16" on macOS; the fix passes. Verified independently that mem::alloc and the ALIGNED operator new both honour alignment correctly, so the defect was the value being passed, not the allocator.
- note:      three green platforms hid it. Linux x64 got 16 from max_align_t by luck of the ABI, and both arm64 legs tolerate unaligned NEON stores — so only MSVC x64 combined a small max_align_t with faulting SIMD. The diagnostic that found it is now permanent: AnimatorSystem::Ctx::ensure asserts all four ozz buffers are 16-aligned and aborts naming the buffer and address, because the alternative is a bare SEGFAULT with no stack.

## BUG-0029 — "Windows hosts do everything" was true of x64 only
- found:     2026-08-26
- class:     build
- where:     modules/assetlib/src/formats/shader_asset.cpp
- symptom:   cooked_format_conformance failed on Windows arm64: "shaderc failed (exit 1)|Error: Unable to load DXC compiler." The whole shader cook fails, so every shader in the project, on that architecture only.
- cause:     profileCookableOnThisHost returned `p < kProfileCount` for all of _WIN32. The D3D compilers bgfx vendors are PREBUILT x86-64 DLLs — d3dcompiler_47.dll, dxcompiler.dll and dxil.dll are all "PE32+ x86-64" — and a native arm64 process cannot load an x86-64 DLL, because the emulator switches whole processes rather than libraries.
- pinned-by: tests/shader_cooker_test.cpp
- lane:      unit
- proof:     mutation FROM macOS — restoring "Windows hosts do everything" fails two table rows ("win arm64 dx11", "win arm64 dx12"); reverted, it passes. `file` confirms all three vendored DLLs are PE32+ x86-64. The rule became a PURE function over (os, arch) so every combination is checkable from any machine — the guard used to answer only for the host it was compiled for, which is why the one platform whose answer was wrong had nothing checking it. profileCookableOnThisHost is now a one-line call into the same table, asserted to agree with it.

## BUG-0030 — the doc-staleness gate could not be seen until after the commit
- found:     2026-08-27
- class:     coverage
- where:     scripts/engine_doctor.py
- symptom:   the Docs workflow failed on a commit whose `engine_doctor check` had passed locally minutes earlier — one ERROR, `src/core/info.md` stale.
- cause:     staleness compared `verified:` against `git log -1` over the covered paths, so a change sitting in the WORKING TREE was invisible. Edit src/core, run check, it passes because the edit is not in history yet; commit, and now it is; push, and CI fails on a doc that was already stale when you looked. A guaranteed round-trip for every commit touching a `hardened` subsystem without a doc bump.
- pinned-by: scripts/engine_doctor.py
- lane:      docs
- proof:     an uncommitted edit to modules/assetlib (hardened, verified one day earlier) now reports "code 2026-08-27 > verified 2026-08-26" locally; reverting clears it. CI is unaffected — it checks out a clean tree, so nothing is ever dirty there.
- note:      the same SHAPE as the docs_status_current fix, arrived at from the other side. That one was a generated file gated on content the commit itself changed; this is a hand-written file gated on history the commit itself creates. In both cases the check was correct and the timing made it unobservable. Bumping `verified:` in the same edit makes the dates equal and the rule is `code > verified`, so the intended workflow passes.

## BUG-0031 — src/core/info.md said the allocator never pools
- found:     2026-08-27
- class:     logic
- where:     src/core/info.md
- symptom:   the doc described mem_counters as "COUNTS + forwards to malloc/free; never pools — ASan/leaks keep working", gated by ENGINE_MEM_COUNT.
- cause:     ENGINE_MEM_ROUTE defaults to 1, so the DEFAULT build routes every allocation into the tagged heaps — the opposite of "never pools". Forwarding to malloc is the ENGINE_MEM_ROUTE=0 path, which the root CMakeLists sets automatically under heap sanitizers. The doc described the fallback as though it were the default.
- pinned-by: src/core/info.md
- lane:      docs
- proof:     found by re-reading the doc rather than bumping its date when BUG-0030's staleness flag pointed at it — which is what the flag is for. The entry now also states the alignment contract that BUG-0028 turned out to hinge on, since nothing described it before.

## BUG-0032 — the probe's verdicts travelled on a channel the thing under test could write
- found:     2026-08-27
- class:     logic
- where:     src/tools/engine_module_probe.cpp
- symptom:   a module could make `engine_module_probe` appear to report an acceptance that never happened, or a completed run that had in fact died, by printing `module 0 ok <path>` and `probe done` on stdout.
- cause:     the probe's machine-readable output contract was ITS OWN STDOUT — `module <i> ok|refused <path>`, terminated by `probe done`. The probe exists to dlopen modules it does not trust; that is the entire reason it is a separate process. Loaded code can print, and it needs no cooperation to do it: a static initialiser runs during `dlopen`, before the host has called anything at all. The forged bytes are indistinguishable from the host's, so a parser cannot tell them apart. Not noise — impersonation, and it works in both directions: an extra `ok` invents an accept, a forged `probe done` makes a truncated run look complete.
- pinned-by: tests/module_abi/tests/gate.rs
- lane:      unit
- proof:     fixture defect 9 (`abi_gate_noisy_stdout`) forges both spellings at both times a module can run code — static initialisation and inside `create` — while its own ABI table is perfect, so it must still LOAD. Observed directly: probing that one module prints THREE `module 0 ok` lines and three `probe done` on stdout (two forged, one real), while the result file holds exactly one `MODULE` record. Any parser reading stdout sees three modules where one was probed. The test also asserts the forged text IS present, so it cannot pass green against a fixture that had quietly stopped printing — asserting nothing was forged in a run where nothing tried.
- note:      the fix generalised into `include/engine/addon_protocol.h` rather than staying local — rule 2, "the result file is the machine channel". `engine_cook_worker` had already learned the same lesson from a different direction ("cookers print freely, so stdout is not a channel") without either being written down as a rule. One tool learning it twice is a coincidence; writing it down is what makes it a protocol.

## BUG-0033 — twelve defects in a shipped package, every one of them at exit 0
- found:     2026-08-27
- class:     build
- where:     src/tools/engine_build.cpp
- symptom:   `engine_build` packaged a game with an unreadable cooked scene, a scene referencing a missing mesh, a mesh referencing a missing texture, two shaders claiming one name, or no shader providing `standard` — and exited 0 with the package written.
- cause:     twelve warning sites, each describing a real defect IN THE SHIPPED GAME, all of them `fprintf(stderr, ...)`. stderr is the human channel, so no caller could see any of it: not the editor, not CI, not a build farm. Only a person reading scrollback — who has no reason to scroll back, because the exit status said the package was fine. The same silently-untextured-build failure the cook worker's trailer was added to prevent, arriving through the packager instead of the IPC channel.
- pinned-by: tests/addon_protocol_test.cpp
- lane:      unit
- proof:     the test runs the real binary and pins its caller-facing contract — `Usage(2)` for a bad command line, `MissingInput(3)` for a directory with no project.json, a stale result file deleted rather than left for a caller that forgets to check the status, and a manifest that round-trips through the real parser and declares `RECORD WARNING` / `RECORD WARNINGS`. Mutation: dropping the stale-result deletion fails the stale-result check.
- note:      HONEST LIMIT — what is pinned is that the tool still CLAIMS to emit warning records and that its exit codes are distinguishable. No test yet packages a deliberately broken project and asserts a `WARNING` record actually appears; that needs a fixture project with a dangling mesh reference, and `package_closure_test` is where it would belong because a real package configures and builds the whole engine tree. Recorded rather than left implied.
- note:      two rules fell out. THE TOOL REPORTS, THE CALLER DECIDES — a warning is not fatal by itself, because whether a missing texture blocks a ship is policy and a packager is not where policy lives; `--strict` is how a caller says otherwise and CI is expected to pass it. And EXIT 1 IS GONE: it meant fifteen different things in that one file, while the protocol reserves 1 as unassigned precisely because it is the exit code of every accident.

## BUG-0034 — a cooked sibling path the result format could not carry
- found:     2026-08-27
- class:     logic
- where:     src/tools/engine_cook_worker.cpp
- symptom:   cooking an asset whose filename contained a newline produced `its result file is unusable: END claims 2 line(s), found 3` in the parent — blaming this process's write. The cook itself had succeeded.
- cause:     `ERROR` went through `oneLine` and `OUTPUT` did not, and `OUTPUT` carries a filesystem PATH. A newline is legal in a POSIX filename and a sibling texture's path is derived from the asset's own stem (`foo.gltf` → `foo_t0.ctex`), so such an asset produced a body one line longer than the writer counted. The frame then failed its own line-count check — correctly — and the diagnosis pointed at the writer instead of the filename.
- pinned-by: tests/cooked_format/tests/cook_result_frame.rs
- lane:      unit
- proof:     mutation — removing the `carryable` guard reproduces the original symptom exactly, and the test reports it as `incomplete: END claims 2 line(s), found 3`. Both halves are pinned: the newline-named asset must produce a result that PARSES and says `fail` with a reason, and an identically-shaped clean asset must still cook to `ok` AND still emit its `OUTPUT` sibling — without that control, a worker that had stopped emitting sibling records at all would pass the first check perfectly while shipping the same missing-sibling bug by another route.
- note:      the fix REFUSES rather than sanitises, and the difference is what the caller does next. The parent OPENS that path and registers it as a cooked sibling, so a mangled path is a DDC record pointing at nothing — a silently missing sibling, the exact class of bug the trailer exists to prevent. A loud precise failure beats a quiet wrong answer. `addon_protocol.h` draws the same line between `record` and `recordExact`; see BUG-0035 for where that line had a hole in it.
- note:      the fixture needs an EXTERNAL image URI. A glTF with its texture embedded as a base64 data URI cooks fine and emits no sibling at all, so it never reaches the record that carries a path — the difference between exercising the bug and cooking a triangle.

## BUG-0035 — recordExact guarded the value and left the key open
- found:     2026-08-27
- class:     logic
- where:     include/engine/addon_protocol.h
- symptom:   `recordExact("MY KEY", "/tmp/x")` emitted `MY KEY /tmp/x`, which every reader in the tree parses as key `MY` with value `KEY /tmp/x`.
- cause:     a record is `KEY value` split on the FIRST space, so a space in the key is not carried — it re-cuts the record. `recordExact` checked the value with `carryable` and did not check the key at all. `carryable` cannot simply be applied to both: a value legitimately contains spaces (it runs to end of line, which is what lets a path with a space in it survive), so the two halves need different predicates. This is the quiet wrong answer `recordExact` was split out of `record` to refuse, hiding inside `recordExact` itself.
- pinned-by: tests/addon_protocol_test.cpp
- lane:      unit
- proof:     mutation — relaxing `usableKey` from `<= 0x20` to `< 0x20` (allowing the space again) fails "recordExact REFUSES a key containing a space"; removing `sanitiseKey`'s space handling fails the `record()` mangling check. The value side is pinned in the opposite direction: `carryable("/tmp/my dir/x")` must stay TRUE, so a later "fix" that refuses spaces outright fails too.
- note:      not reachable from this tree — every call site passes a string literal. Pinned because "no caller does that yet" is not a property of the function, and this one's whole job is to refuse rather than mangle. `record` mangles the space to `_` instead of refusing: it returns void and its contract is that it always emits, so refusing would mean silently dropping a warning, which is worse than an ugly key.

## BUG-0036 — a spec that used whitespace decoratively
- found:     2026-08-27
- class:     logic
- where:     include/engine/addon_protocol.h
- symptom:   the manifest example in the header showed column-aligned records (`ID       engine_module_probe`, `RECORD   MODULE`) while every tool emits exactly one space.
- cause:     the example was formatted for readability in a header whose stated contract is that a third-party tool can "reimplement it from the format documented in the comments". A writer built from the aligned version emits padded keys; every reader splits on the FIRST space and hands back a value with leading blanks. The bytes were always right — the specification of them was not.
- pinned-by: include/engine/addon_protocol.h
- lane:      docs
- proof:     the emitted manifest was read back byte-wise (`od -c`) and confirmed single-spaced, so this was the comment disagreeing with the code rather than a behaviour change; the example now matches, and states the one-space rule explicitly alongside the reason a value may then contain spaces freely. `addon_protocol_test` independently searches for `RECORD WARNING\n`, so a padded key would now fail a test as well as a reading.
- note:      the same shape as BUG-0031 — a document that was wrong about code that was right. It counts here because this header is not documentation ABOUT a format, it IS the format's specification, which the Rust conformance suites exist to keep honest by reimplementing from it.

## BUG-0037 — the doc gate counted documentation as code, so fixing docs broke the gate
- found:     2026-08-28
- class:     coverage
- where:     scripts/engine_doctor.py
- symptom:   editing `src/runtime/docs/bgfx-includes-in-runtime.md` — a document, changing no code — produced `ERROR src/runtime/docs/info.md: tier hardened requires a fresh doc, but it is stale (code 2026-08-28 > verified 2026-08-25)`, which is commit-blocking.
- cause:     `last_change()` asks git whether anything under a doc's `covers:` paths has changed, with no filter on file type. `covers:` names DIRECTORIES, and directories contain their own documentation — `src/runtime/` holds `src/runtime/docs/`, `src/render/` holds `issues.md`. So any markdown edit under a covered path marks that subsystem's `info.md` stale. At `tier: hardened` that is an ERROR, and the only route to a clean gate was re-verifying a subsystem nothing had touched.
- pinned-by: scripts/engine_doctor.py
- lane:      docs
- proof:     mutation — setting the exclusion pathspec back to empty reproduces the ERROR exactly; restoring it clears it. Found while correcting the renderer documents: the gate refused a change whose entire content was making documentation more accurate.
- note:      the incentive is the reason this rates an entry rather than a tweak. This script exists to keep documents honest, and it was charging a tax on doing so — improving a doc cost strictly more than neglecting it, and the cheapest way to a green gate was to stop touching documentation. A gate that penalises its own purpose is worse than no gate.
- note:      it had been mis-firing before today, silently. Applying the exclusion dropped the standing warning count from **12 to 8** with no code change and no `verified:` bump: four documents were flagged stale purely because a sibling document had been edited. Four false alarms is how a warning list stops being read.
- note:      third in a family. BUG-0027 was a generated file gated on content the commit itself changes; BUG-0030 was a gate that could not be seen until after the commit. All three had a correct RULE measuring the wrong thing, and all three were found by the gate misbehaving on a real edit rather than by reading it.

## Open — recorded so they are not forgotten

- **A portability regression can now sit on main for up to a day.** The gating
  macOS leg runs on every push; the four Linux/Windows legs run nightly and on
  `workflow_dispatch`. Deliberate — wall clock was 11m27s set entirely by
  Windows, for a verdict macOS had already given, and the test lane inside it is
  20 seconds. All five legs are `experimental: false`, so the nightly is a hard
  failure rather than an amber note, and the full matrix is one dispatch away
  before anything that matters. Revisit if a regression ever survives a night.
- **The push path is now bounded by TSan (~6m50s), not macOS (~4m53s).** Left
  there on purpose: the sanitizer lanes are the highest-yield minutes in the
  whole workflow — BUG-0001, 0002 and 0003 all came from them — so moving them
  off the push path to save under two minutes would undercut the reason most of
  this ledger exists. If pushes need to get faster again, TSan is the next
  candidate and the cost is stated here rather than discovered.
- **The build is ~110 link steps with a 97% compiler-cache hit rate.**
  Compilation is already free; the six minutes are ~85 test executables each
  statically linking the whole engine, and sccache does not cache links. Trimming
  targets cannot move that — the levers are a shared engine library or fewer,
  larger test binaries, both with real tradeoffs.

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

- **The shader cooker could not spawn shaderc on Windows, and said so.** The
  result file the harness now prints carried the answer verbatim: "standard
  [metal mask 0] vertex: shader compilation is not wired for Windows hosts yet".
  Our own deferred stub — the POSIX branch uses `posix_spawn`, the Windows branch
  returned that string.

  Implemented with `CreateProcessW`, mirroring the POSIX path step for step. Two
  decisions worth keeping: no shell, because shader paths routinely contain
  spaces and handing a command string to `cmd.exe` turns argument splitting from
  a formatting question into a correctness one; and stdout/stderr redirected to
  the same FILE the POSIX side uses rather than a pipe, because a pipe must be
  drained while the child runs or shaderc blocks once the buffer fills — a
  deadlock that appears only on shaders with many diagnostics, which are exactly
  the ones whose output matters. The quoting rules are the same ones
  `worker_win32.cpp` already worked out; `CommandLineToArgvW` treats backslashes
  as literal EXCEPT before a quote, where each must be doubled.

- **`Path::exists()` and `.exe`, and an ambiguous message that hid it.** Fixed
  last commit; confirmed here — the cmat/ctex tests went from failing to 4
  passing, leaving only the shader one above.

- **The instrumentation found it on its first run: a hardcoded `/tmp`.**
  `input_test` reached all eight phases and hung in "recorder round-trip", which
  used `const char* recPath = "/tmp/input_test_session.irec"`. There is no `/tmp`
  on Windows, `fopen` returned nullptr, and the code walked straight into
  `fread(&e, sizeof(e), 1, f)` on a null `FILE*`.

  The 120-second hang was not the path bug. MSVC's DEBUG CRT reports invalid
  parameters through `_CrtSetReportMode`, whose default for `_CRT_ASSERT` is
  `_CRTDBG_MODE_WNDW` — a MODAL DIALOG. On a runner nobody clicks it, so a
  one-line wrong assumption became an unkillable silence. Three fixes, because
  they are three different faults: the path (`fs::temp_directory_path()`), the
  `CHECK` that recorded a failure and then continued into UB (it returns now),
  and the dialog default — `testwd::quietenCrtDialogs()` routes CRT reports to
  stderr and drops the Windows Error Reporting handoff for every test. A test
  that violates a CRT precondition should die loudly; a dialog is not loud, it is
  just slow.

  Getting `_set_abort_behavior` backwards on the first attempt is worth noting:
  passing 0 for both arguments would have suppressed the abort MESSAGE too,
  silencing the one part that makes an abort diagnosable while fixing the part
  that hangs.

- **`Path::exists()` on an extensionless name is false on Windows.**
  `harness::cook_worker()` looked for `engine_cook_worker`; the binary is
  `engine_cook_worker.exe`. Every cmat/ctex test then reported "ENGINE_BUILD_DIR
  unset or engine_cook_worker missing" — a message naming two causes when the
  truth was neither: the directory was right and the worker was present under a
  name the function never asked for. `std::env::consts::EXE_SUFFIX` exists for
  this and is `""` everywhere else. The message now distinguishes the two cases,
  because an ambiguous diagnostic cost a round on its own.

- **A 120-second silence is not evidence.** `input_test` timed out on Windows
  having printed NOTHING — not even the banner on the first line of `main()`.
  That absence was the clue, not the mystery: ctest redirects stdout, redirection
  makes it BLOCK-BUFFERED, and the timeout kill discards the buffer. Measured
  rather than assumed — the same program killed mid-run yields **0 lines**
  buffered and **2 lines** unbuffered.

  So 30 of 65 tests could have hung anywhere and told us nothing. All 65 now set
  `_IONBF` before their first print. That is the load-bearing half.

  The other half is `tests/test_watchdog.h`, for a hang inside a call that prints
  nothing at all: a thread fires BELOW ctest's own timeout, names the last phase
  reached, and `_Exit(97)`s. `_Exit` rather than `abort()` deliberately — on MSVC
  `abort()` can raise the CRT error dialog or hand off to Windows Error
  Reporting, and a CI runner then waits on a dialog nobody will click, turning a
  diagnosable hang into a longer one. Verified by making a probe hang on purpose
  under redirection: exit 97, phase trail intact, last phase named.

  The pattern is the same one that ended the five-round `pix3_win.h` hunt: after
  the second guess, spend a round buying facts instead.

- **The last Windows failure was a sample that CANNOT work there as written.**
  `hot_reload_game` is a MODULE library that deliberately links nothing and
  resolves every engine symbol from the `engine_host` executable at load time.
  That is a Unix idiom: macOS allows it with `-undefined dynamic_lookup`, ELF
  shared objects permit undefined symbols and the loader binds them from the
  executable. A Windows DLL must resolve every symbol AT LINK TIME, so it failed
  with LNK2019 on flecs' globals — plus a second, separate problem in the same
  line: the `__imp_` prefix, because `flecs.h` declares `dllimport` unless
  `flecs_STATIC` is defined and the sample links no flecs target to inherit it
  from.

  Making it work on Windows is design, not a flag: `engine_host` would have to
  EXPORT the symbols a module may use (Windows exports nothing from an EXE by
  default, and `WINDOWS_EXPORT_ALL_SYMBOLS` covers DLLs only) and the module would
  link the generated import library. **Windows KITS will hit exactly this**, so it
  is worth designing once rather than patching in a sample.

  Off by default on MSVC until then, which is the option the first Windows port
  attempt asked for in as many words. What made that a defensible call rather
  than a convenient one: `-k 0` proved it was the ONLY remaining failure on both
  legs — zero compile errors, one failed target — so the engine, every tool, every
  shader and all 76 tests build on x64 and ARM64 Windows.

- **`-k 0` paid for itself on its first run.** The round after adding it reported
  the COMPLETE Windows picture instead of one line: three root causes across both
  legs, all independent, all fixable together. Six earlier rounds had each
  delivered exactly one. Nothing about the code changed to make that possible —
  only the flag.

- **The module ABI could not be compiled by the toolchain most likely to build a
  module.** Three SDK headers — `contract.h`, `engine_api_client.h`,
  `game_module.h` — wrote `__attribute__((visibility("default")))` directly. MSVC
  spells it `__declspec(dllexport)`, so every module entry point was "C3861:
  'visibility': identifier not found" plus a C2059/C2143 cascade. A C ABI whose
  entire purpose is to be dlopen'd from a shared library, unbuildable on Windows.
  Now `ENGINE_MODULE_EXPORT` in its own header, because those three are
  independent SDK entry points (`contract.h` includes nothing but `<stdint.h>`)
  and each has to stay usable alone. Verified the symbol is still exported after
  the swap rather than assuming it.

- **A C99 compound literal in a C++ test.** `nav_test.cpp` passed
  `(const float[3]){3, 1, 3}`; GCC and Clang accept that in C++ as an extension,
  MSVC rejects it (C4576). A named local costs nothing — the temporary only had to
  outlive the call. `script_host.h` had already reached the same conclusion in a
  comment, which is a sign the extension was known to be a trap and the knowledge
  had not travelled.

- **Our blake3 arch test disagreed with blake3's own.** `CMAKE_SYSTEM_PROCESSOR`
  is `ARM64` on Windows-on-ARM, `aarch64` on Linux and `arm64` on macOS — and
  CMake's `MATCHES` is CASE-SENSITIVE, so `MATCHES "arm64|aarch64"` failed on
  Windows only. The NEON kernel was never compiled, while `blake3_impl.h` derived
  `BLAKE3_USE_NEON` from `__aarch64__ || _M_ARM64 || _M_ARM64EC` — correct on MSVC
  ARM64 — and `blake3_dispatch.c` duly called it: "unresolved external symbol
  blake3_hash_many_neon".

  The lesson is not the missing `TOLOWER`. It is that the compiler already
  answers "is this AArch64?" exactly, and we answered it again, worse, by string
  comparison against an OS-specific spelling. `blake3_neon.c` cannot simply be
  compiled unconditionally either — it includes `<arm_neon.h>` with no guard — so
  the two predicates have to agree, and only one of them has the facts.

- **Ninja stops at the first error, and that turned a port into a treadmill.**
  Six consecutive CI rounds each revealed exactly ONE Windows portability bug —
  a missing macro, then a POSIX function, then another — because the build stops
  scheduling work at the first failure and a fix can only be validated by pushing
  it. Six round-trips for six independent one-line problems, none of which
  depended on the others.

  The build now passes `-k 0`, so the whole graph is attempted and the summary
  step gets the complete list. Nothing is hidden — the build still exits
  non-zero; it just stops rationing the diagnosis. This is the same lesson as
  `check_std_includes.py`: when the loop is "fix one, wait, learn one", the thing
  to fix is the loop.

- **`setenv`/`unsetenv` are POSIX, in four test files at once.** MSVC has neither;
  it spells the first `_putenv_s`. The genuinely different half is UNSETTING:
  POSIX has `unsetenv(name)`, while on Windows `_putenv_s(name, "")` REMOVES the
  variable rather than defining it as empty. Code reading `getenv(x) && *x` cannot
  tell those apart, but `getenv(x) != nullptr` can — and that is exactly what
  `harness::skips_are_failures` and `resolveTexTarget` do, so getting it wrong
  would have made an env-var test pass for the wrong reason on one platform.
  Worse than not compiling. Behind `testenv::set`/`unset` now, and verified by
  running the affected tests with the variables PRE-SET in the environment.

- **DXC is x86-64 only on Windows too, and bgfx's CMake assumes otherwise.**
  `bgfxToolUtils.cmake` appends both `s_5_0` (DXBC) and `s_6_0` (DXIL) for any
  WIN32 target, and `tools/bin/windows/dxcompiler.dll` is a `PE32+ x86-64` DLL
  with no ARM64 build — so every shader in the project failed on Windows-on-ARM
  with "Unable to load DXC compiler". The identical fact already forced the
  equivalent guard on the COOKER side for Linux arm64
  (`profileCookableOnThisHost`), where `libdxcompiler.so` is likewise x86-64.
  Two independent places had to learn the same thing, which is a hint the
  question "can this host emit DXIL?" deserves one home rather than two.

- **Fixing bx's arch detection uncovered the same assumption one file over.**
  `simd_t.h` read `#if defined(__SSE2__) || (BX_COMPILER_MSVC && (BX_ARCH_64BIT ||
  _M_IX86_FP >= 2))` — "MSVC and 64-bit" meaning "x86-64", which was fair until
  Windows on ARM64. With `BX_ARCH_64BIT` finally correct there, bx concluded SSE2
  was available on an ARM target and included an x86 intrinsics header:
  "emmintrin.h(20): fatal error C1189: This header is specific to X86, X64,
  ARM64, and ARM64EC targets".

  Before the arch fix, `BX_ARCH_64BIT` was 0 here — so ONE BUG WAS MASKING THE
  OTHER, and correcting the first was what made the second reachable. Worth
  expecting whenever a long-wrong platform predicate is finally fixed. Gated on
  `BX_CPU_X86` now; MSVC ARM64 falls through to the scalar paths exactly as it
  did before, because the NEON arm wants `__ARM_NEON__` and MSVC provides
  `_M_ARM64` with `<arm64_neon.h>` instead.

- **`popen`/`pclose` are POSIX spellings.** The editor's terminal panel used them
  directly; MSVC provides `_popen`/`_pclose` from the same `<stdio.h>` and not the
  unprefixed names, so the panel was "C3861: 'popen': identifier not found" on
  every Windows build. Aliased once at the top of the header rather than ifdef'd
  at the two call sites — the panel wants "run a command and read its output",
  which is one capability with two spellings. A sweep for the rest of that family
  (`fork`, `execvp`, `mkstemp`, `ftruncate`, `gettimeofday`) found only
  `worker_posix.cpp`, which is a deliberate file-per-platform split with a
  `worker_win32.cpp` sibling.

- **bx believed ARM64 Windows was a 32-bit target, and that set the whole SDK
  back to Windows XP.** Its arch test lists `__x86_64__`, `_M_X64`,
  `__aarch64__`, `__64BIT__`, `__mips64`, `__powerpc64__`, `__ppc64__`,
  `__LP64__` — and **`_M_ARM64` is absent**. MSVC on ARM64 defines `_M_ARM64` and
  never `__aarch64__` (a GCC/Clang macro); the neighbouring CPU test had the same
  hole, using `_M_ARM`, which is the 32-BIT ARM macro.

  A few lines below, that decision picks the Windows API level:
  `BX_ARCH_64BIT` -> `_WIN32_WINNT 0x0601` (Windows 7), else `0x0502` (XP SP2).
  So every bgfx translation unit on ARM64 Windows compiled against an XP-era SDK
  surface — `NTDDI_VERSION 0x05020000`, below `NTDDI_VISTA` — and the SDK gated
  out every Vista+ declaration. Which is why `renderer_d3d12.cpp` could include
  `<shlobj.h>` *successfully* and still report `SHGetKnownFolderPath` and
  `KF_FLAG_DEFAULT` as undeclared.

  THREE PATCHES TO `pix3_win.h` FAILED before this, because that header was never
  the problem. What ended it was measuring instead of guessing: a `#pragma
  message` printing the version macros, which showed `NTDDI_VERSION=0x05020000`
  on arm64 and `0x06010000` on x64 **in the same run**. The lesson is not about
  `_M_ARM64` — it is that after two failed guesses at the same file, the next
  commit should buy facts rather than attempt a third.

- **`ENGINE_ABI_FINGERPRINT` was built from a macro MSVC does not define.** It
  used `__VERSION__` (GCC/Clang only), so on Windows the kit-loading ABI gate did
  not merely lose precision — it FAILED TO COMPILE, cascading C2143/C2059 through
  `module_loader.h` and `kit_host.h`. The one check whose entire job is "refuse a
  module built by a different compiler" was unbuildable on the compiler most
  likely to differ. Now `ENGINE_ABI_COMPILER`: `_MSC_FULL_VER` on MSVC (patch
  granularity, because the STL's layout can move between toolset patches),
  `__VERSION__` elsewhere, and an explicit `"unknown-compiler"` rather than a
  fingerprint that might silently match across two different compilers.

- **The `/MTd` that appeared on no command line.** `engine_cook_worker.exe` died
  with "LNK2038: mismatch detected for 'RuntimeLibrary': value 'MTd_StaticDebug'
  doesn't match value 'MDd_DynamicDebug'", and grepping every compile command in
  the build log for `/MTd` found ZERO — because with CMP0091 NEW the runtime comes
  from a target PROPERTY, not a flag, so it never appears on a command line at
  all. The only visible trace was assimp's `assimp-vc145-mtd.lib`, and that `mtd`
  is merely assimp's naming convention — a red herring that cost a round.

  The culprit was ozz: `ozz_build_msvc_rt_dll` defaults OFF, after which
  `compiler_settings.cmake` does an unguarded
  `set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")` in its
  own directory scope, which SHADOWS a cache value pinned at the top level.
  Pinning the cache is not enough when a subdirectory can set a plain variable
  over it; the option has to be flipped as well. Two vendored libraries
  (Jolt, ozz) do exactly this, which suggests treating it as the norm.

- **`texture2D` inside flow control is a hard error on D3D.** `fs_triangle.sc`
  sampled the base colour inside a ternary and the normal map inside an `if`,
  both on uniform conditions. HLSL compiles `texture2D` to `Sample()`, which
  derives its mip level from screen-space derivatives — a GRADIENT operation —
  and D3DCompile refuses those under flow control: "X3129: gradient-based
  operations must be moved out of flow control to prevent divergence". The
  build passes `--Werror`, so every Windows leg failed on a shader that compiles
  cleanly on Metal, SPIR-V, GLSL and ESSL.

  Both samples are now hoisted above their branches. This costs nothing and
  changes no result: the renderer ALWAYS binds a texture to those stages
  (`ctx.whiteTex` / `ctx.flatNormalTex`, see `opaque_pass.cpp`), so `u_texFlags`
  says whether the fetch is MEANINGFUL, not whether it is legal. `texture2DLod`
  would have been the tidier fix and is not available for the GL profiles: bgfx
  defines it only for `BGFX_SHADER_LANGUAGE_GLSL >= 130`, and this build also
  compiles `--profile 120` and `100_es`, so using it everywhere would have traded
  a D3D failure for a GLSL one.

  Hoisting fixed those two and revealed a sharper error underneath: "X3570:
  gradient instruction used in a loop with varying iteration". That one is the
  SHADOW fetch, inside the light loop — and the loop breaks on `i >= count`, which
  the compiler treats as varying even though `count` is a uniform. Hoisting the
  call out would cost nine taps per pixel with shadows off, and any branch around
  it brings X3129 back, so the fetch itself is now explicit-LOD
  (`SHADOW_FETCH`), guarded to HLSL where `texture2DLod` exists. That is also
  more correct everywhere: a shadow map has no mip chain, so screen-space
  derivatives were never meaningful.

- **Jolt links against a different C runtime than everything else.**
  `USE_STATIC_MSVC_RUNTIME_LIBRARY` defaults to ON on MSVC and sets
  `CMAKE_MSVC_RUNTIME_LIBRARY` to `MultiThreaded$<$<CONFIG:Debug>:Debug>` —
  `/MTd` — while every other target used the default `/MDd`. `engine_cook_worker`
  died with "LNK2038: mismatch detected for 'RuntimeLibrary': value
  'MTd_StaticDebug' doesn't match value 'MDd_DynamicDebug'" and LNK1169. One CRT
  per program is not negotiable: objects built against different runtimes cannot
  share a heap, allocations or iostreams. Forced OFF, and the runtime is now
  pinned explicitly at the top level so a vendored library flipping the default
  cannot do this quietly again.

- **We forced a reduced `<windows.h>` on every third-party library.** The
  top-level CMakeLists had `add_compile_definitions(NOMINMAX
  WIN32_LEAN_AND_MEAN)`, and `add_compile_definitions` applies to every
  subdirectory added afterwards — so bgfx compiled with our lean header. Its
  vendored `directx-headers/pix3_win.h` calls `::SHGetKnownFolderPath` and uses
  `KF_FLAG_DEFAULT` without including `<shlobj.h>`, because it expects the full
  header, and `renderer_d3d12.cpp` failed with "C2039: 'SHGetKnownFolderPath' is
  not a member of '`global namespace''".

  The two defines are not the same kind of thing. `NOMINMAX` suppresses a MACRO
  and is safe to impose on anyone; `WIN32_LEAN_AND_MEAN` changes WHAT GETS
  INCLUDED, and a library gets to decide its own include surface. Ours now
  states the choice per translation unit, next to the `<windows.h>` it applies
  to — which is what the fifteen guarded `#define`s were already doing.

- **`::getpid()` does not exist on Windows.** `shaderc_invoke.cpp` built a
  unique scratch filename from the process id; MSVC has `_getpid()` in
  `<process.h>` and no `<unistd.h>` at all. Wrapped in a `currentProcessId()`
  helper rather than an ifdef at the call site: the caller wants "something
  unique to this process", which is a capability rather than a platform question.

- **Linux arm64 could not cook a single shader, because DXC is x86-64 only.**
  `profileCookableOnThisHost` allowed `dx12` on any Linux host — "d3d4linux covers
  DXIL" — but the DXC it loads is a VENDORED PREBUILT:
  `bgfx/tools/bin/linux/libdxcompiler.so`, which `file` reports as
  "ELF 64-bit LSB shared object, x86-64". There is no arm64 build. So arm64
  accepted the profile, handed the stage to shaderc, and shaderc died with
  "dlopen failed ... Unable to load DXC compiler" — failing the WHOLE shader cook,
  on one architecture only, which is exactly why x86-64 Linux was green.

  The guard asked which OS and needed to ask which OS AND ARCHITECTURE. A
  capability that depends on a checked-in binary's arch cannot be inferred from
  the platform alone.

- **The test fixture's own allocator raced, under the sanitizer written to catch
  races.** `audio_provider_asan_test` tracked live blocks in an unsynchronised
  `std::map`, and the provider is EXPLICITLY allowed to allocate off the game
  thread — the ABI hands it `parallelFor` and tells it to decode there. TSan
  reported races inside libc++'s red-black tree (`__tree_remove`,
  `__find_equal`, `__insert_node_at`). Left alone, a corrupted tree would
  eventually have surfaced as "the provider freed a pointer we never handed out":
  the fixture accusing the code it was written to vindicate. One mutex over the
  whole record, because `live`, `peak` and `blocks` have to move together.

- **`/Z7` won the flag argument and sccache lost anyway.** Adding a later `/Z7`
  did stop assimp's `/Zi` from taking effect — C1041 disappeared — but `/Zi` was
  still ON THE COMMAND LINE, and sccache parses the command line: it expected a
  PDB, `/Z7` meant none was written, and it failed with "failed to open file
  assimp-vc145-mtd.pdb". The flag had to be ABSENT, not merely outvoted, so it is
  now removed at source by `assimp__no-zi-debug-info.patch`.

- **`render_status` claimed staleness a shallow clone cannot know.** `check_docs`
  already refused to (`changed = "" if SHALLOW`), and `is_shallow()`'s own
  docstring says "refusing to make the claim beats emitting confident nonsense" —
  but the status RENDERER computed it anyway. CI checks out `fetch-depth: 1`, so
  `git log` sees only the tip and every subsystem's "code last changed" came back
  as TODAY, flipping the ⚠️ marker on twenty rows that were not stale. Two halves
  of one tool disagreeing about what it is allowed to know.

  That marker is also why the status gate kept failing after dates were
  normalised away: ⚠️ is git-derived but is not a DATE, so it survived date
  normalisation. The gate now blanks both freshness columns and the marker, and
  mutation-verified that it still catches real structural drift — a changed tier
  and a dropped entry from a `tests:` list both fail it.

- **The cook worker exits 0 when the COOK fails.** `return f.good() ? 0 : 65` —
  the exit code reports whether the result FILE was written, because the verdict
  travels inside that file, which is what the real pipeline reads. The
  cooked-format test checked `status.success()` instead, so it was asking "did
  the worker run?" while believing it asked "did the cook succeed?". On Linux
  arm64 the worker exited 0, printed ten ordinary warnings and produced no
  `.cshader`, and the best the test could say was "no output (shaderc
  unavailable?)" — a guess, while the machine-readable answer sat in a file three
  lines away. Verified by hand: a malformed `.shader` yields exit 0 with
  `RESULT fail` and the real parse error.

- **MSVC takes the LAST flag, and two vendored libraries relied on that.**
  The top-level build rewrites `/Zi` to `/Z7` precisely to avoid PDB contention,
  and assimp's own `CMakeLists.txt:357` appends `/Zi` back inside its directory
  scope — so `/Zi` won and `C1041: cannot open program database ... please use
  /FS` killed four TUs at once, **with `/FS` already on the command line**. Fixed
  by a per-target `/Z7`, which lands after directory flags. ozz does an
  unconditional `add_compile_options(/WX)` with no opt-out, which turned one
  warning in `blending_job.cc` into a build failure on arm64 Windows; a
  per-target `/WX-` disables it by the same mechanism. Both expressed in our own
  CMakeLists rather than as patches, because the FLAG ORDER is the mechanism and
  it belongs next to the rewrite it defends.

- **`maRealloc` copied the NEW size out of the OLD block — a heap over-read on
  every sound decode.** The host services interface has no realloc, deliberately,
  so the shim did alloc + copy + free and bounded the copy by the new size. Its
  own comment said that was "safe only while miniaudio uses this path to GROW",
  which is exactly inverted: growing is the unsafe direction. 64 KB → 128 KB read
  64 KB past the end.

  On macOS the over-read landed inside the tagged heap's mapped region and
  nothing ever went wrong. In a Linux container it hit an unmapped page and
  SIGSEGV'd inside `ma_decoder__full_decode_and_uninit`. Every block now carries
  its size in a 16-byte prefix and the copy is bounded by the MINIMUM, which is
  the only bound correct in both directions. Mutation-verified: restoring the old
  bound brings the segfault straight back.

  **Why it survived so long is the more useful part.** ASan runs on every push
  and would have caught this instantly — but `audio_abi_conformance` is excluded
  from sanitizer builds, so the one test that exercises this code is the one test
  the sanitizer cannot see. A comment two lines above the bug described the
  hazard correctly and drew the wrong conclusion from it; nothing executable
  disagreed.

  There was a second layer of luck: the engine's host allocator is TLSF over
  large mappings, so a 64 KB over-read lands inside a 2 MB block and disturbs
  nothing. Even with the sanitizer watching, the REAL allocator gives it nothing
  to guard.

  Now closed by `tests/audio_provider_asan_test.cpp`, which is C++ (so no
  sanitizer exclusion), dlopens the same shipping module, and backs host `alloc`
  with plain `malloc` so ASan has redzones. It asserts the decode actually GREW a
  block — otherwise it would pass while exercising nothing — and that every
  pointer freed was one it handed out, which is the invariant a size-prefixed
  allocator can break. Verified both ways on macOS: clean with the fix, and with
  the old bound restored it reports
  `heap-buffer-overflow ... READ of size 65536 ... maRealloc` — the exact defect,
  on the machine where it hid for months, with no Linux runner involved.

- **The overrun counter measured the CI runner, not the mixer.** A Linux runner
  has no sound card: ALSA prints "cannot find card '0'" and then returns a
  working 48 kHz device anyway. Its callback cadence is whatever the scheduler
  felt like, so `now - last > period * 2` fired 9 times and failed "NO callback
  overruns under a 2000-command burst" — an assertion about our mixer, failing on
  a fact about the machine. An overrun means "we missed a HARDWARE deadline", so
  with no hardware there is nothing to miss. `ENGINE_AUDIO_NO_HARDWARE=1` (set by
  ctest, never by a shipped game) both permits the null-backend fallback and
  stops the timing assertions. The first attempt at this flag meant "we used the
  null backend" and was too narrow to ever trigger — ALSA had already succeeded.

- **`verified:` dates from a UTC+5:30 machine are "in the future" to a UTC
  runner.** Three docs stamped 2026-08-25 from a machine where it was, failing on
  a runner where it was still the 24th — a hard error, on the gating leg, for a
  timezone. The check now compares against UTC with a day of slack; it exists to
  catch a transposed year, not to police which side of midnight someone lives on,
  and a 2027 stamp is still rejected. The same clock skew flipped `Contract
  errors: 0` to `3` inside the generated ENGINE_STATUS.md, which is why the
  status gate failed too — live-check counts are now excluded from that
  comparison for the same reason the stale count already was.

- **The miniaudio provider published `engineInit` as a plain `bool` and the audio
  callback read it.** TSan caught it on the CoreAudio callback thread against the
  write in `maCreate`. The ORDERING was already correct — the flag is set before
  `ma_device_start`, exactly as its comment says — but ordering in the source is
  not a happens-before edge, and the callback thread already exists by then
  (`ma_device_init` creates it), so nothing made the write visible. Now a
  release store paired with a single acquire load at the TOP of the callback,
  which orders everything else `maCreate` published.

  A second race hid behind a comment: `expectedPeriodNs` sat under
  "Callback-local, touched only by the audio thread" and is computed in
  `maCreate`. Grouping a cross-thread field under a comment asserting it cannot
  be one is how it stayed invisible.

- **`docs_status_current` could not be satisfied, and failed three runs in a
  row.** `ENGINE_STATUS.md` carries git-derived data — a per-subsystem "code last
  changed" column and a stale-doc count — so regenerating BEFORE a commit and
  AFTER it give different answers by construction: the working-tree edits are not
  in history yet, and the moment they are, every doc covering them moves. Stale
  docs read 7 before the commit and 14 after, same tree, same script. Committing
  the file invalidated it.

  A generated file cannot be gated on content that changes because of the commit
  that contains it. The gate now compares STRUCTURE — subsystems, tiers, docs,
  test counts — and normalises dates away; freshness stays with `engine_doctor
  check`, which computes it live. Two comments already in that function record
  the same lesson from two other causes, which is a sign the check needed the
  rule stated rather than patched a third time.

- **The reference audio provider's clock ran slow under load, and blamed the
  suite for it.** Its stand-in driver thread was `sleep(period); samples +=
  frames;` — so the effective sample rate was `frames / however long that sleep
  ACTUALLY took`, and `sleep` guarantees only a lower bound. On GitHub's 3-core
  macOS runner, with the Rust harness running several tests at once, the clock
  advanced at under half real time and the suite's own "the sample clock and the
  host clock agree within 0.5x-2.0x" check failed — taking two tests down with
  it, on the GATING leg, while passing 15/15 on a 12-core laptop.

  The assertion was right and the reference was wrong. It now derives
  `samplesPlayed` from `Instant::elapsed()`, which is what real hardware does:
  the device clock keeps running whether or not the callback thread was
  scheduled promptly — that is what an underrun IS. Reproduced deterministically
  by injecting a 3x sleep overshoot (identical failure, identical message), and
  the fix passes 3/3 under the same injection.

  Two lessons worth keeping. A fixed sleep followed by an assertion about what
  happened during it encodes the developer's machine into the pass condition.
  And the first attempted fix — poll until the clock moves AT ALL — traded a
  starvation failure for a QUANTISATION one (2.7 ms of samples vs 6.3 ms of
  host): a provider advances in buffer-sized chunks, so over a one-buffer window
  the quantisation is the measurement.

- **Four files used `std::memcpy`/`std::strlen` without `<cstring>`.** libc++
  pulls it in transitively and libstdc++ does not, so both Linux legs failed to
  compile while macOS never noticed. CI only ever reported the FIRST one, since
  the build stops there — the other three were found by grepping for the pattern
  rather than by waiting for three more red runs.

- **Windows kits are blocked on design, not a flag.** `hot_reload_game` is a
  MODULE that deliberately links nothing and resolves engine symbols from the
  host executable at load — a Unix idiom. A Windows DLL must resolve every symbol
  at link time, so it fails LNK2019 on flecs' globals. Making it work means
  `engine_host` EXPORTING the symbols a module may use (Windows exports nothing
  from an EXE by default, and WINDOWS_EXPORT_ALL_SYMBOLS covers DLLs only) and
  the module linking the generated import library. Both Windows legs otherwise
  report ZERO compile failures and build all 76 tests.
- **The ledger is not updated by the gate that checks it.** `engine_doctor`
  validates entries that EXIST — ids unique, class known, `pinned-by` present —
  and structurally cannot notice an entry that should exist and does not. Three
  commits fixing seven defects landed before this was spotted, and only because
  someone went looking. Nothing automated will catch the next one; the discipline
  is "a fix and its entry land together", and that is a human rule.
- **The backfill is incomplete.** Twelve `issues.md` files hold roughly 2,400
  lines of documented, already-fixed defects. The entries above are today's
  findings plus the recent ABI and audio work. Older subsystems — the renderer's
  R10–R21, the asset cookers, assetlib — are not yet indexed, and until they are
  the histogram under-reports.
