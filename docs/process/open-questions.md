---
status: reference
---
# Open — known, unpinned, and deliberately visible

**Things that are wrong, or are a deliberate cost, and have no regression test.**

Split out of `bug-ledger.md` when that file passed 1000 lines: numbered entries
moved to `bugs/`, and this moved here. It is the other half of the same honesty —
the ledger says what is fixed and proven, this says what is known and not.

Unlike a `BUG-NNNN` entry, nothing here is machine-checked. That is the point:
an item earns a number and a `pinned-by` when someone fixes it, and until then a
gate would have nothing to enforce. The risk is that this list rots instead —
so an item that turns out to be FIXED is a finding, exactly like a stale doc.


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
