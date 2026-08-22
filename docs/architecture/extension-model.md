---
status: decided
---
# The Extension Model — Plugins, Kits, Add-ons, Providers

**How code that is not the engine attaches to the engine, and what each way
costs.**

There are four ways, along two independent axes. Three of them *extend* the
engine; the fourth *replaces* part of it.

> **What exists today**
>
> | | State |
> |---|---|
> | **Plugin** | built, in use — `src/plugins/`, `IEnginePlugin` |
> | **Kit** | built, in use — `include/engine/engine_api_table.h`, hot-reloaded `.so` |
> | **Provider** | **live** — `engine_audio_provider.h`, implemented by `src/audio/miniaudio_provider.cpp`, consumed by `audio_plugin.h`. Built both statically and as a standalone `.so` the conformance suite loads |
> | **Add-on** | **not built.** Designed here, nothing implements it |
> | **API registry** | **not built.** Designed here |
>
> The tier table and the swap recipe below are decisions. The unbuilt rows are
> intent, marked as such wherever they appear.

---

## 1. Why this document exists — the ABI saga

Every rule below was paid for. The short version, because the rules are
arbitrary-looking until you know what they cost.

### 1.1 The promise

Kits compile against `engine_api_table.h`. The engine publishes one struct of
function-pointer groups; a Kit binds to it at load and calls through it. The Kit
never links an engine symbol, so the engine can be rebuilt, refactored or
reorganised without touching the Kit.

### 1.2 The bug

The client shim gated binding on two equality checks:

```c
if (t->structSize != sizeof(EngineApiTableV1)) reject;   /* wrong */
g_eapiOk[c.idx] = (c.have == c.want);                     /* wrong */
```

`structSize != sizeof(...)` rejects a Kit built last week **because the table
got bigger** — not incompatible, *bigger*. And `have == want` means a group
improved to v2 no longer satisfies a module asking for v1, so improving a
subsystem locks out every module built before the improvement.

Appending `jobs`, `memory` and `drawSubmit` invalidated all three Kits and the
game module **wholesale**. An engine whose ABI breaks every consumer on every
append does not have an ABI; it has a version-locked build.

### 1.3 Why `>=` alone would have been worse

The obvious fix is `>=`. On its own it is *actively dangerous*, and this is the
part to internalise.

Groups are stored **inline**, not behind pointers:

```c
struct EngineApiTableV1 {
    uint32_t structSize;
    EngineApiCoreV1  core;      /* offset 8   */
    EngineApiInputV1 input;     /* offset 128 */
    ...
};
```

An old Kit computes `input`'s offset from *its own* headers. If `core` ever grows
from 120 to 128 bytes, every later group shifts — and the old Kit reads `input`
at offset 128, now the middle of `core`. It gets a plausible function pointer to
the wrong function, calls it with the wrong arguments, and corrupts something.
**Silently.** No load failure, no version mismatch, just wrong behaviour
surfacing far from its cause.

So `>=` needs offsets that never move, and frozen offsets need `>=` or the gate
rejects anyway. **Neither half works alone.**

### 1.4 The fix, both halves

**Sizes** are frozen at build time in the header:

```c
ENGINE_API_FROZEN(EngineApiCoreV1, 120);
```

**Offsets** are frozen by [`tests/api_abi_compat_test.cpp`](../../tests/api_abi_compat_test.cpp),
because a *reordered* group keeps every size intact and still breaks every Kit —
the static asserts would pass and the world would still burn.

| group | offset | size | | group | offset | size |
|---|---|---|---|---|---|---|
| `core` | 8 | 120 | | `ui` | 536 | 48 |
| `input` | 128 | 120 | | `nav` | 584 | 32 |
| `physics` | 248 | 64 | | `draw` | 616 | 40 |
| `audio` | 312 | 32 | | `jobs` | 656 | 32 |
| `assets` | 344 | 128 | | `memory` | 688 | 48 |
| `anim` | 472 | 64 | | `drawSubmit` | 736 | 24 |

The test does the real thing rather than a proxy for it: it declares the table's
*old* shape (a verbatim prefix, stopping at `draw`), lays it over the live
760-byte table, and asserts the function pointers resolve identically. That is
literally "a v1 Kit reading a v5 host." It also asserts the one direction that
must keep **failing**: an older host does not satisfy a newer module.

The same pass found `static bool warned[7]` guarding nine groups — an
out-of-bounds write on every warning past the seventh.

### 1.5 What this costs, permanently

**A shipped group can never change size or position.** There is no way to add a
function to `EngineApiPhysicsV1`. Ever.

Growth happens one way only: **append a new group**. More physics means
`EngineApiPhysicsV2` on the end of the table, while `physics` stays exactly where
it is, forever, at 64 bytes. The table only grows; groups are immutable once
shipped; old Kits keep reading the old group at the old offset.

The tax is real — eventually there is a `physics` and a `physics2` and dead space
in a group nobody uses. It is the same bargain COM, Vulkan and the Linux syscall
table all made, and paying it in struct padding is enormously cheaper than paying
it in "recompile everything."

---

## 2. Axis one — isolation

|  | **Plugin** | **Kit** | **Add-on** *(unbuilt)* |
|---|---|---|---|
| Linkage | static, in the binary | dynamic `.so` | separate **process** |
| Boundary | C++ (`IEnginePlugin`) | C ABI table | IPC |
| Access | everything, internals included | only what the table exposes | only what the protocol exposes |
| Version tolerance | none — same build | **v1 Kit on a v5 engine** | protocol-versioned |
| Hot reload | no, rebuild the engine | **yes** | restart the process |
| A crash takes down | the engine | **the engine** | only itself |
| Latency | zero | ~ns per call | µs–ms, serialised |
| Per-frame work | yes | yes | **no** |
| Written by | the engine team | game teams, third parties | anyone, including untrusted |
| For | engine capability | reusable gameplay systems | tools |

Two things to pull out of that table.

**Kits do not protect the engine from crashes.** They are in-process. A null
deref in a Kit is a segfault in the game. What the C ABI buys is *version*
safety — a v1 Kit surviving to v5 — which is a completely different property
from *fault* safety. "Dynamically loaded behind a C ABI" sounds safe, and only
one kind of safe is true.

**That gap is the entire argument for Add-ons.** An asset importer parsing a
malformed FBX from the internet must not be able to kill the editor.
Out-of-process is the only real answer — and it is affordable precisely because
tools do not participate in the frame. An importer costing 40 ms of IPC round
trip is fine; a physics engine costing that is not.

> **The process boundary is affordable exactly where the frame boundary is not.**

### Choosing a tier

```
Does it run per-frame?
├─ no  ──────────────────► ADD-ON   (crash-proof, untrusted input, tools)
└─ yes
   ├─ does it need engine internals? ──► PLUGIN (ships with the engine)
   └─ no ──────────────────────────────► KIT    (hot-reload, version-stable)
```

---

## 3. Axis two — direction

Consider the audio provider. It is a dynamic `.so` behind a C ABI — Kit-shaped
by isolation. It is not a Kit, and calling it one confuses the model:

- **A Kit consumes.** The engine publishes a table; the Kit calls into it. *The
  Kit depends on the engine.*
- **A Provider implements.** The provider publishes a table; the **engine** calls
  into it. *The engine depends on the provider* — and the provider, ideally, does
  not know the engine exists.

Same isolation tier, opposite direction, and the ABI rules apply for **opposite
reasons**:

| | frozen layout protects |
|---|---|
| Kit table | old *consumers* against new engines |
| Provider table | old *engines* against new implementations |

> **Plugins, Kits and Add-ons extend the engine. Providers replace parts of it.**

The axes genuinely cross. An out-of-process provider is coherent — a heavyweight
offline path tracer for audio or lighting bakes, IPC-connected, allowed to crash.
It simply cannot be anything on the frame path.

---

## 4. The swap recipe

How to make a subsystem replaceable. Generalised from
[`audio-provider.md`](../guides/audio-provider.md), which is the worked example.

**1. Find the rate boundary.** Every subsystem has an inner loop far faster than
the frame — the audio mixer at 375 Hz, the physics solver at a fixed 60 Hz plus
substeps, thousands of draws per frame. **That loop must never cross the ABI.**
If it does, allocation, locking and unwinding become joint problems on both
sides of it.

**2. Give the implementor the hot loop.** For audio that meant the provider owns
the device and the real-time thread. That single decision pushes latency, buffer
ownership, thread synchronisation and DSP entirely to their side — the four
things impossible to retrofit later.

**3. Model the scene, not the features.** There is no `setHRTF` or
`enableRayTracing`, because spatial audio, Atmos, binaural and propagation all
need the *same three inputs*: emitter poses, listener pose, acoustic geometry.
Feature-shaped interfaces grow with every technology ever invented. Scene-shaped
ones do not grow at all.

*The test before adding a function: could a competing commercial implementation
implement it? If not, it describes our implementation rather than the scene.*

**4. Batch at frame rate.** `updateEmitters` takes an **array**. Hundreds of
moving emitters is one call, and anything that did not move sends no row.

**5. An escape hatch for the un-modelable.** `setParam(object, nameHash, value)`,
forwarded uninterpreted. Wwise RTPCs and FMOD parameters already work this way.
Experimental knobs go here and the engine never learns what they mean.

**6. Hand over host services.** The provider gets the engine's jobs, tagged
allocators and clock (`EngineAudioHostServices`) so it does not build a second
thread pool that oversubscribes the cores, or a second allocator invisible to the
memory budget. The hot loop's thread stays theirs; everything else uses ours.
Make the suite *count* the calls — services that are handed over but never
checked test nothing, since a provider can take the struct and call `malloc`.

**7. Address content by name, not only by bytes.** The gap that nearly shipped.
`createSound(bytes)` assumes the engine holds the data, which is true for sample
players and false for every event system: Wwise and FMOD ship banks and resolve
`Play_Gunshot` through a designer's graph. If an interface cannot express a name,
no commercial adapter can implement it — and a shared hash function has to ship
*in the header* so both sides compute it identically.

**8. The conformance suite is the specification.** A provider that compiles and
fails the suite is not a provider. Written in Rust deliberately: the claim the
ABI makes is that a non-C++ language can implement it, so the suite tests that
claim mechanically. A C++-only suite could not.

**9. Freeze the layout; `structSize` everywhere** — section 1, applied in advance
rather than in hindsight. Two details that are easy to miss:

- An **out-parameter** inverts the check. The *caller* sets `structSize` and the
  provider writes only what fits, because there the newer party is the writer —
  the one direction frozen layout does not protect.
- The suite must pin **both sides**. A Rust suite asserting its own transcription
  is blind to the C header moving underneath it, so the C side needs a
  translation unit that actually includes the header (or its static asserts never
  compile) and pins **offsets**, which `sizeof` cannot see.

### Applying it to the other two *(neither designed yet)*

**Physics.** Boundary is the solver step. The engine sends batched body
creation/destruction and applied forces; the provider returns a batched array of
transforms. Broadphase, island solver and substeps never cross. The hard part is
**determinism**: a physics ABI that does not pin step order and float behaviour
makes providers non-interchangeable for anything networked or replay-based.

**Renderer.** Boundary is draw submission. The engine produces culled render
items; the provider consumes an array and owns its command buffers, barriers and
passes. The hard part is that resource creation and shader compilation are
provider-specific in ways `createSound` is not — it needs a portable
material/shader representation, which is the genuinely difficult problem and the
one bgfx currently solves for us at a ceiling we have already hit.

---

## 4b. Hosts — the category that is not an extension

A **host** is the application that drives the engine: the ImGui editor,
`engine_player`, `engine_host`, a future Qt or Rust editor, a studio's own
launcher. A host is not an extension — it does not attach *to* the engine, it
*contains* one. But it belongs in this document, because "could someone else
write the host?" is the same question as "is anything vendor-locked?"

**The rule: the editor is a consumer of the SDK, not a layer of it.** Anything
the ImGui editor needs, every other host needs too — so nothing a host requires
may live under `src/editor/`.

Verified state of that rule:

| | Status |
|---|---|
| Sources outside `src/editor/` including ImGui or editor headers | **none** (two comments mention ImGui; no includes) |
| `editor` target | an **executable**, the only thing linking ImGui, one-way on `engine_runtime` |
| SDK header install | excludes `src/editor` explicitly |
| `engine_cook`, `engine_cook_worker`, `engine_build`, `engine_project` | link **only `engine_core`** — no runtime, no window, no ImGui |
| `engine_core` | genuinely GPU-free: zero `#include` of bgfx/GLFW/SDL (shaderc runs as a child process) |
| `assetlib` | links SQLite + blake3 only; no engine dependency |

So a Qt or Rust editor can link `engine_core` **today** and get cooking, the
DDC, the material/shader/texture cookers, package closure and project
scaffolding, with no window system at all.

The window seam (`wsi::`, `runtime/platform/window_ops.h`) moved out of
`src/editor/` for exactly this reason: it had made multi-window support an
ImGui-editor privilege, forcing any other host to reinvent it against GLFW
directly.

**Driving the runtime works too.** `engine_runtime` used to link GLFW
unconditionally in both backends, because `runtime/input/input_system.h`
included `<GLFW/glfw3.h>` and branched on the backend throughout — so an SDL3
build linked GLFW, and so did a host supplying its own window. That header now
goes through `wsi::` and names no backend at all, which leaves each windowing
library reachable from exactly two TUs (its `IPlatform` and its `window_ops`),
both already selected by one CMake branch. `libglfw3.a` is in the SDL3 link line
before the change and absent after.

So the SDK now offers a host three things independently: a **GPU-free** layer
for tools (`engine_core`), a **runtime** that links one windowing library of its
choosing, and a **per-window seam** for driving more than one window. None of
them requires ImGui, and none of them requires the editor.

---

## 5. Adding a *new* subsystem — the API registry *(not built)*

The API table is fixed at compile time: twelve groups, frozen offsets. A studio
writing a voxel system, a destruction system or a dialogue engine **cannot add
it**, and their Kits have no way to reach it.

The fix needs no new ABI concepts — the same frozen-layout rules apply per
registered table:

```c
/* A subsystem publishes its own table under a name. */
EngineApiResult engineRegisterApi(uint64_t nameHash, uint32_t version,
                                  const void* table, uint32_t structSize);

/* A consumer asks for one, with the same >= gate. */
const void* engineFindApi(uint64_t nameHash, uint32_t minVersion);
```

A studio ships `"acme.destruction"` v1; their Kits look it up, get `NULL` on an
engine that does not have it, and **degrade instead of failing to load**. The
engine never learns what destruction is — and, the important part, never has to
release a new version to accommodate them.

Which is what "not vendor lock-in" means in practice: not that you can leave, but
that you can extend without asking.

---

## 6. The rules, in one place

Everything crossing a dynamic boundary — Kit table, provider table, registered
table, Add-on protocol:

- **Fixed-width types only.** No `bool`, no `size_t`, no plain `enum`, no
  bitfields, no `long`.
- **No C++ types cross.** No `std::`, no references, no virtuals, no templates.
- **`structSize` on every struct**, set by the producer, checked by the consumer.
- **Append-only.** Never reorder, never resize, never repurpose a field.
- **Version with `>=`**, never `==`. Host must satisfy module, not equal it.
- **No exception or panic may escape any function.** In Rust: `panic = "abort"`
  or `catch_unwind` at every entry point.
- **Absence is normal.** Missing capability returns a clean refusal
  (`E_UNSUPPORTED`, `NULL`) and the caller degrades. It is not an error to abort
  on.
- **Freeze offsets in a test**, not only sizes in a static assert. Sizes catch a
  growth; only offsets catch a reorder, and a reorder breaks everything while
  every static assert still passes.
