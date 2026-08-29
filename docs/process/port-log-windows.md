---
status: reference
---
# The Windows and cross-ISA port log

**Twenty-five findings from taking an engine that only ever compiled on macOS and
making it build on Windows x64, Windows arm64, Linux x64 and Linux arm64.**

Every one of these is FIXED. This is not a list of open questions — it is the
record of what a port actually costs, kept because the next port will rhyme with
it and because several of these took a full CI round-trip each to see.

It was split out of `open-questions.md` on 2026-08-29, where it had grown to more
than half a file titled "Open — known, unpinned, and deliberately visible" while
being neither open nor unpinned. A document whose title stops being true of its
contents is one nobody can act on: an item that reads as a live gap sends someone
to fix what is already fixed.

## How to read this

Six of these are also `BUG-NNNN` entries with regression tests, because they were
defects in the engine. The rest are TOOLCHAIN findings — MSVC does not define a
macro, bx believes ARM64 Windows is 32-bit, DXC ships x86-64 only — which have no
test to pin because the fix is a build file, and the value is entirely in knowing
they exist before hitting them again.

The recurring shapes, which are the reason this is worth keeping as a narrative:

* **POSIX spellings assumed to be universal.** `setenv`, `popen`, `getpid` — each
  found separately, each the same lesson, none of them the last one.
* **Architecture detection that agreed with itself and nothing else.** Our blake3
  test, bx's `simd_t.h`, bgfx's DXC rule, and bx's ARM64-is-32-bit belief were
  four instances of one assumption in four files.
* **Silence read as success.** A 120-second timeout, a cook worker exiting 0, a
  shader cooker that could not spawn `shaderc` and said so into a channel nobody
  parsed.
* **Flag arguments MSVC settles by taking the last one**, which two vendored
  libraries were quietly relying on.

---

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

- **`/Z7` won the flag argument and sccache lost anyway.** Adding a later `/Z7`
  did stop assimp's `/Zi` from taking effect — C1041 disappeared — but `/Zi` was
  still ON THE COMMAND LINE, and sccache parses the command line: it expected a
  PDB, `/Z7` meant none was written, and it failed with "failed to open file
  assimp-vc145-mtd.pdb". The flag had to be ABSENT, not merely outvoted, so it is
  now removed at source by `assimp__no-zi-debug-info.patch`.

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
