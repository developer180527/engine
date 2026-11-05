---
status: plan
covers:
  - include/engine/
  - src/runtime/platform/
---
# ----DRAFT------ The engine's public C API and what we promise about it

> **----DRAFT------ Nothing here is built, linked or promised.** Written
> 2026-11-05, for review. The headers are in
> [`engine-api-draft/eng/`](engine-api-draft/eng/engine.h), deliberately outside
> `include/`, so nothing can depend on them by accident. Every file carries the
> `----DRAFT------` tag and `ENG_API_DRAFT`. Both come off at version 1.0, and
> the promise below starts then.

## 1. The idea

**One C API is the engine's only public boundary** (decided by the owner,
2026-11-06, DR-0028: C, so that any language can bind to it and it stays stable
across compilers). Programs, kits, the editor,
the tools and every language package use it. The C++ layer is a thin wrapper over
it, the way librhi is built.

```
 your game        kits       Python  Swift  JS/WASM  Rust     editor, tools
     │             │            └──────┴──────┴───────┘             │
     │             │              generated from the headers        │
     └─────────────┴──────────────── eng* C API ────────────────────┘
                                         │        ← the promise stops here
                          core (private) + providers (chosen)
```

Because everything stands on the same API, keeping that API stable keeps
everything built on it stable.

## 2. What we promise, from 1.0

Within one major version, across every minor version:

| Promise | Meaning |
|---|---|
| **Source** | A program or kit that compiles today compiles against any later minor version. |
| **Binary** | A kit or program built today runs on any later minor version, without rebuilding. |
| **Behaviour** | A documented behaviour does not change. A bug fix that some code might rely on goes in the release notes. |
| **Data** | A scene or project saved today loads in every later version, including later major versions, through migrations. |

**Covered:** the `eng*` functions, their structs and enums, the common components
(`core.*`), the engine's service tables (`engine.*`), and the scene format.

**Not covered:**
- everything else in the engine: core internals, flecs, bgfx, all of `src/`;
- the C++ provider contract for renderers (DR-0006 stands: a renderer's shape is
  still changing, so renderers are compiled from source against each version);
- anything marked experimental (§6);
- the binary layout of the C++ wrapper's classes: it is headers over the C API,
  so it is compiled into your program.

**A binary is tied to its platform and major version.** A kit built for macOS
arm64 does not run on Windows; that is what rebuilding for each platform is for.
Struct sizes differ on 32-bit web, where pointers are 4 bytes. Generated bindings
use the right layout for each target.

## 3. The rules that make the promise possible

Few rules, all checkable. Most are already paid for in
[`extension-model.md`](../architecture/extension-model.md) and already used by
librhi, so one set of rules covers both libraries.

1. **Functions are never changed or removed.** A new need is a new function.
2. **Structs that can grow start with `struct_size`.** Zero is the default for
   every field, so `ENG_INIT` plus the fields you care about is always enough.
   The engine reads only `struct_size` bytes and treats the rest as zero.
3. **For a struct the engine fills, the caller sets `struct_size`** and the engine
   writes only that much. This covers the direction rule 2 cannot.
4. **Value types and structs passed in arrays can never grow** (`EngVec3`,
   `EngFieldDesc`). They carry `reserved` words that must be zero.
5. **Enums are only appended.** Fields and arguments are fixed-width integers,
   because a C `enum`'s size varies. Treat an unknown value as "not for me".
6. **Handles are plain values with a generation, and zero means none.** A stale
   handle is refused with `ENG_ERROR_STALE_HANDLE`, not a crash.
7. **Every fallible call returns `EngResult`,** and `engGetLastErrorMessage` says
   why, naming things. No exceptions cross. Malformed input is refused.
8. **Nothing that other languages cannot read:** no C++ types, varargs, bitfields,
   unions or `bool`, and no implicit padding (checked on 64-bit and 32-bit).
9. **Strings are UTF-8 and copied** by whoever keeps them. Memory is freed by
   whoever allocated it.
10. **Each comment states its thread.** Windows are main-thread only.

**Today's API breaks rule 8 and would need to change:** `engineUiText` is varargs,
and `EngineRaycastHit` and many calls use `bool`.

## 4. The two ways to reach it

**A program links the library.** The shared library exports `eng*` symbols and
nothing else, with everything private hidden; static linking works too.

**A kit asks by name.** A loaded kit exports one function, `engModuleEntry`, and
receives `get_proc`. A small generated stub calls `get_proc("engCreateEntity")`
and so on at load, so kit source calls `engCreateEntity` like any other function.
A kit linked into a game (the only path on iOS, consoles and the web) skips the
stub and calls the symbols directly. The source is the same.

**Why by name instead of today's table.** Today's `EngineApiTableV1` must freeze
every group's size and offset forever, which is why `physics2` exists. Asking by
name has no layout to freeze, and a new function can sit next to its relatives.
Vulkan and OpenGL work this way.

## 5. What the draft contains

| Header | Calls | Holds |
|---|---|---|
| `eng_base.h` | 2 | versions, results, value types, handles |
| `eng_engine.h` | 17 | create, pump, `engStep`, `engRun`, log, memory, jobs, crash notes |
| `eng_window.h` | 26 | capabilities, monitors, windows, native handles, embedding, events |
| `eng_view.h` | 8 | which camera appears where; offscreen capture |
| `eng_input.h` | 7 | actions, custom input sources, replayable intents |
| `eng_world.h` | 31 | entities, component schemas, queries, systems, change stream, blobs |
| `eng_components.h` | 0 | the common vocabulary (first few, to show the shape) |
| `eng_asset.h` | 8 | load by name, or make meshes, textures and materials in code |
| `eng_module.h` | 3 | kits, providers, and services of any kind |

That's 102 functions. It should stay small: **gameplay features are services,
not core calls.** Physics is the worked example (`EngPhysicsV1`). Audio, animation,
navigation, debug draw and networking follow the same pattern, and each can grow
or be absent without touching core.

**Who owns the loop:** your program. `engStep` does one frame and returns; it never
pumps events and never blocks on a minimised window. `engRun` is a convenience
built from the same calls, and is refused where a loop may not block (web, iOS).

**Windows:** [`examples/apollo.c`](engine-api-draft/examples/apollo.c) is the test
case: fullscreen windows per monitor, a smaller DSKY window, one simulation, a
real switch panel through an action, and the same cameras in one window when
there is only one screen. A fullscreen-only game sets the mode and
`ENG_WINDOW_NO_USER_MODE_CHANGE`.

**Kits get smaller.** [`examples/kit.c`](engine-api-draft/examples/kit.c) is a
whole kit: one description and the same calls as a program. No `IEnginePlugin`, no
engine or flecs includes, no compiler match. A component's schema is checked by
name when it registers, which replaces the compiler fingerprint and layout hash.

**Components are plain bytes.** That is what lets Python and JS see them as arrays
without copying, and what removes the kit-unload crash class (component hooks
compiled into a kit). The cost: a kit component cannot hold a `std::vector`. It
keeps that in its own storage, keyed by entity.

**Writing is declared.** Writing through a raw column would be invisible to the
change stream, so a query declares `ENG_ACCESS_WRITE` and every row in that column
counts as changed. A provider may see a row that did not really change, and never
misses one that did.

**Proposed after review (2026-11-06), in the headers, marked "proposed":**
- **Marked writes.** `ENG_ACCESS_WRITE` alone would make physics, which writes
  every body's transform each step, report every row: the renderer would
  re-upload all objects every frame though most are asleep, which is the
  opposite of what the stream is for (WO-019). `ENG_ACCESS_WRITE_MARKED` plus
  `engMarkChanged(iter, term, first, count)` reports only the rows a system says
  it changed. `ENG_ACCESS_WRITE` stays as the safe default.
- **Merged changes.** Between two reads an entity appears at most once per
  stream: changed many times is one CHANGED, added then removed is nothing, and
  an entity id is not reused while a stream could still report it.
- **Engine-owned variable-length data.** A component is still plain bytes, but a
  field may be an `EngBlob`: bytes the engine owns (`ENG_FIELD_BLOB`,
  `engCreateBlob`, `engGetBlob`, `engSetBlob`), saved with the scene, migrated,
  freed with the component and readable from every language. Without it, a list
  (contacts for `CollisionEvents`, WO-052; a script path) lives in a kit's own
  storage, which nothing saves, migrates or sees.
- **More field types**, appended before anyone binds the enum: 8- and 16-bit
  integers, VEC2, VEC4 (and colours), MAT4.

## 6. How it changes over time

- **Versions.** `0.x`: nothing promised, any minor may break. `1.0`: the promise
  starts. A minor version only adds. A major version is rare, and the previous
  major's functions stay available for at least one major version.
- **Experimental work** goes in a service with an `experimental.` prefix, so using
  it is visible in your own code, and it can change freely. Moving out is how
  something gets promised.
- **Services version by name:** `engine.physics.v1` only grows at its end. A real
  break is `engine.physics.v2`, and a provider can offer both.

## 7. Keeping the promise honest

A promise nobody checks decays, so each of these fails CI:

1. **One description, generated from the headers:** a script reads them through
   Clang into `api.json`. Bindings, the kit stub, docs and the checks all use it,
   so agents can read it too.
2. **A record of 1.0.** Every function, signature, struct offset and enum value is
   compared with the recorded release. Removing or moving anything fails; adding
   passes. (`kit_abi_headers.py` and `api_abi_compat_test.cpp` are the start.)
3. **Old binaries stay in the tests.** A kit and a program built against 1.0 are
   kept and run against every later build. This is the only direct check of the
   real promise.
4. **The exported symbol list** contains `eng*` and nothing else.
5. **A conformance suite in another language.** Python drives the API with no C++
   help, which proves the claim the way the Rust suite does for audio providers.

## 8. Other languages

| Language | How | Note |
|---|---|---|
| **C++** | a header-only wrapper: RAII handles, `Result`, typed components | hand-written, small |
| **Python** | generated `cffi` module; query columns as NumPy arrays | per-entity loops in Python will be slow; batches and kits are the fast path |
| **Swift** | a generated module map plus a thin Swifty layer; `engAdoptNativeWindow` for SwiftUI | `engStep` from a display link |
| **JS** | the engine compiled to WASM with generated TypeScript; `engStep` from `requestAnimationFrame` | needs the WebGPU renderer; 64-bit entity ids are `BigInt` |
| **Rust** | generated `-sys` crate plus a safe layer | matches librhi's plan |

A system written in Python or JS can register like any other, but only runs on the
main thread and never in parallel. Each language's own rules decide that (Python's
GIL, the browser's threads).

Tools get **a separate library** with its own promise (`engine_tools`: import,
cook, build), so nothing development-only can reach a shipped game.
`engLoadModule` is the one development-time call in this API. A shipped game keeps
the symbol, returns `ENG_ERROR_UNSUPPORTED`, and has no loader inside.

## 9. What happens to today's API

| Today | Becomes |
|---|---|
| `include/engine/engine_api.h`, `EngineApiTableV1` | kept and filled, unchanged, until the question in §11 is answered |
| `EngineRuntime`, `engine.h` | internal; the C++ wrapper replaces them for users |
| `IEnginePlugin` | internal, for first-party code only (`provider-abi.md` seam C) |
| `game_module.h` | replaced by `eng_module.h` |
| `IPlatform`, `IToolWindow`, `wsi::` | one internal backend behind `EngWindow`; the editor uses the same public windows |
| `EngineAudioProviderV1` | stays as the audio provider's contract, found as a service |

**Order, each step leaving a working engine:**
1. Agree on this draft and the open questions.
2. `engStep` as the primitive, and main-window control on today's internals. This
   is useful immediately and proves the frame and window sections.
3. The world half over flecs, with the `engine_runtime` tests driving it.
4. Multiple windows and views.
5. `api.json`, the generated stub, and one real kit ported. Then Python.
6. Every first-party kit, the player and the editor on the API. **Using it
   ourselves for real is what earns 1.0.**
7. 1.0: the tag comes off, and the record of §7.2 is taken.

## 10. What was checked on this draft

Compile-only, since nothing is implemented. Run by hand, not in CI.

- All headers and both examples compile (re-run 2026-11-06 with the proposals) with `-Wall -Wextra -Wpedantic -Werror`
  under Clang for AArch64 Linux, x86-64 Linux, x86-64 Windows (MSVC target) and
  32-bit WASM. Headers also compile alone as C99, and together as C++17.
- All 36 structs show no implicit padding on macOS arm64, x86-64 Linux and WASM.
  That check found three real cases, now fixed. On the MSVC target Clang reports
  padding between fields but not at the end of a struct, so that one gap remains
  for Windows.
- **Not checked:** real MSVC, whether any of this can be implemented as designed,
  whether generated bindings are pleasant to use, performance.

## 11. Questions for the owner

1. ~~**One C API for everything, with C++ as a wrapper?**~~ **Answered
   2026-11-06: yes** (DR-0028).
2. **Asking by name instead of a frozen table?** Recommended: yes (§4).
3. **Old kits:** `kit-abi.md` promises the old table is filled indefinitely, but
   only our own kits exist so far. Recommended: **move them all, then retire the
   old table before 1.0**, while no third party depends on it.
4. **Plain-bytes components?** Recommended: yes, despite losing `std::vector` in
   kit components (§5).
5. **Prefix:** the draft uses `eng` / `Eng` / `ENG_`, so it cannot clash with
   today's `engine*`. Fine, or does the engine get a proper name first? A name is
   much harder to change after 1.0.
6. **Providers beyond audio:** the draft keeps renderers on source only (DR-0006)
   and allows physics to become a binary service later. Is that the right line?
