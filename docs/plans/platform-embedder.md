---
status: plan
covers:
  - src/runtime/platform/
---
# The platform embedder

> **Status: plan. Nothing here is built.** Revised 2026-08-30 after review; §0.1
> records what the first draft got wrong, including a misquoted source line.
> §5's ABI sketch is **provisional** and is expected to change under E1–E3.

## 0. The decision, and the invariant under it

**The host owns the window, the event loop and the surface. The engine is a
guest.** That inversion is what lets one engine run inside `engine_player`, the
editor, a dedicated server with no display, and a SwiftUI shell on an iPad —
without the engine knowing any of them exist.

The rule that decides every question below, and the one to reach for when a
specific decision is unclear:

> **The engine must not assume ownership of any host-controlled resource or
> scheduling mechanism.** No implicit event pumping. No blocking to wait for a
> surface. No window creation on the embedded path. No hidden present loop. No
> assumption that a platform object lives as long as the engine does.

"Use a C ABI" is the mechanism, not the invariant. A C ABI that still pumps
events has changed nothing.

Named after Flutter's embedder, which is the clearest existing statement of the
same idea. **This is not new work** — it is `renderer-program.md` P1 and P2 under
a better name, plus the ABI that makes them reachable from outside C++.

### 0.1 Corrections to the first draft

Recorded rather than silently fixed, because this document's value rests on its
citations being checkable:

* **A quote was abbreviated and presented as verbatim.** §2.3 rendered the
  minimise loop as two lines when the source has four. The dropped
  `waitEvents(0.1)` matters: the loop **blocks on events**, it does not spin, and
  the draft's word "spinning" was wrong about the mechanism. The severity
  argument is unchanged — a guest that can block its host indefinitely is not a
  guest — but the description of *how* was wrong.
* **§2.2 and §2.3 were sourced to one line.** They are two different defects at
  two different lines: the unconditional pump at `runtime_frame.cpp:23`, and the
  conditional minimise loop at `:31`. The first happens every frame; the second
  only when the framebuffer is 0×0.
* Two review claims were checked and are **not** correct: `sdl3_platform.h` does
  exist (with `sdl3_platform.cpp` and `window_ops_sdl3.cpp`), and the loop is at
  `runtime_frame.cpp:31` as cited, not in `runtime.cpp`.

## 1. What already exists

This document was going to argue for building a seam. Most of it is there:

| | state |
|---|---|
| `IPlatform` — window, events, framebuffer size, cursor | ✅ `src/runtime/platform/platform.h` |
| Three implementations | ✅ `glfw_platform.h`, `sdl3_platform.h`, `headless_platform.h` |
| The host can **inject** one | ✅ `EngineRuntime::init(cfg, std::unique_ptr<IPlatform>)` |
| A real second consumer uses it | ✅ `profiler_frames.cpp:27` passes `HeadlessPlatform` |
| The host owns the frame loop | ✅ `while (engine.frameBegin(dt))` in player and host |
| `wsi::` models *many* windows, with an `InputSink` | ✅ `window_ops.h` |

So the question is not "should we have an embedder seam". It is **"why can the
one we have not carry an iPad?"**

## 2. The five gaps

### 2.1 It is a C++ vtable, not a C ABI

`IPlatform` is `provider-abi.md`'s **seam C**: virtual functions, `std::string`,
`std::unique_ptr`. An implementor needs our compiler, our standard library
version, our headers, and must not let an exception escape. A SwiftUI shell
cannot implement that; neither can a Qt tool or a Rust host.

### 2.2 The engine pumps its own events — every frame

`runtime_frame.cpp:23`, unconditional:

```cpp
m_platform->pollEvents();
```

On iOS and in any retained UI toolkit the **OS owns the event loop and calls
you**; there is no point at which you may pump. This is the routine case and it
happens on every frame.

### 2.3 The engine can BLOCK inside a frame call — when minimised

`runtime_frame.cpp:31-35`, verbatim this time:

```cpp
while ((fbw <= 0 || fbh <= 0) && !m_platform->shouldClose()) {
    m_platform->waitEvents(0.1);
    m_platform->pollEvents();
    m_platform->framebufferSize(fbw, fbh);
}
```

Conditional — only a 0×0 framebuffer reaches it — and it *waits* rather than
spins. Neither fact rescues it: a host that owns its frame loop cannot tolerate
the engine blocking for any duration under any condition, and on iOS the watchdog
makes that concrete.

**E1 deletes this loop, which creates a question §3.6 has to answer**: once
blocking is gone, a 0×0 surface is an ordinary runtime state and `tick` needs
defined behaviour for it.

### 2.4 There is no lifecycle model

Nothing says *suspended*, *the surface went away*, or *the GPU went away*. On iOS
backgrounding **revokes GPU access**. §4 rewrites this as three state machines,
because the draft's single one conflated them.

### 2.5 The engine creates the window

`platform_factory.cpp` constructs a `GlfwPlatform` or `Sdl3Platform` by default.
An embedder already has a window and a `CAMetalLayer` or `HWND` to go with it.

## 3. The contracts that must be settled BEFORE any ABI is frozen

`provider-abi.md` §2's eight rules cover struct layout and versioning. They do
not cover any of the following, and a C ABI frozen without them is one where
every host discovers the rules by hitting bugs. **§9 says a frozen ABI is
forever; this section is what makes freezing it safe.**

### 3.1 What `tick` does, exactly

"One frame" is four separable things: consume queued input, advance simulation,
render, present. Two conforming hosts could legally drive the engine in
incompatible ways while both believing they conform.

The contract must state which of the four `tick` performs, whether any are
separately callable, and — for a dedicated server — whether rendering is skipped
or absent.

### 3.2 What `dtSeconds` means

Wall-clock elapsed, simulation time, or a requested timestep? And what happens
when a host supplies a hostile value: an enormous `dt` after a breakpoint, zero,
negative, or two ticks with no render between them.

Today `frameBegin` clamps to `0.05f` and hands the first frame `0.0f`. Once the
host owns the loop that clamp becomes part of the **contract**, not an internal
detail: state whether `dt` is trusted, clamped, or a hint.

### 3.3 Threading

**Entirely absent from the draft, and the most dangerous omission.** Nothing said
whether the calls are serialized, whether events may arrive during `tick`, or
whether the host may call from more than one thread. Without it, an
implementation quietly becomes a cross-thread synchronization API that nobody
designed.

Baseline to state explicitly: **all calls serialized on one host-chosen thread,
non-reentrant, unless a specific function is documented otherwise.** Then say
what is legal during a callback — can a lifecycle change be delivered while a
`tick` is in progress, as an OS event naturally would be?

### 3.4 Event ownership and ordering

`engineEmbedderEvent(handle, const EngineInputEventV1*)` — does the engine copy
immediately or retain the pointer? Are events ordered? May they arrive while
suspended?

The clean answer is: **consumed synchronously, pointer never retained, delivery
ordered, and calls serialized per §3.3.** Pointer lifetime across a C ABI is an
ABI bug rather than a compile error, which is exactly the class worth closing by
specification.

Also unresolved: whether this ABI *is* `wsi::InputSink` in C form, or translates
into it. That decides where coordinate conversion, device identity and
platform-specific interpretation live.

### 3.5 One clock, and why `nowNanos` was wrong

The draft offered `nowNanos` as an optional host capability *and* passed
`dtSeconds` into `tick` — two timing authorities.

Worse, the engine already has a canonical one. `modules/hid/include/hid/hid.h:64`
is explicit: `timeNs` is *"monotonic ns, same clock as `hid::nowNs()`"*, and the
input system's record/replay determinism depends on every timestamp in the
pipeline coming from that one clock.

A host-provided clock stamping embedder-pushed events, diverging from
`hid::nowNs()`, is precisely BUG-0007 and BUG-0008 again — two clocks that were
assumed to be one.

**Recommendation: drop `nowNanos`.** `dt` into `tick` is sufficient. If absolute
host timestamps are ever needed, define them as unifying with `hid::nowNs()`
rather than as a separate embedder concept.

### 3.6 Zero-size surface

After E1 removes the blocking loop, what does `tick` do at 0×0? Skip rendering
but advance simulation? Refuse the tick? Render offscreen? Currently the engine
answers by waiting, and E1 deletes that answer without supplying another.

### 3.7 Suspended: what still runs

"Do not touch the GPU" is stated. Whether **simulation** continues is not. A
mobile host may stop calling `tick` entirely; another may keep driving for
non-rendering work. Without a rule, two conforming hosts behave differently.

### 3.8 Pixels or points

`width`/`height` plus `dpiScale` — are the dimensions physical pixels or logical
points? On the motivating consumer this is not academic: get it wrong and input
coordinates, viewport dimensions and render-target sizes drift apart on every
Retina display. Define one canonical **pixel** space for rendering and a separate
logical space where UI needs it.

### 3.9 Who presents

The host owns the surface — but does the engine acquire the drawable, encode,
and present internally, or does it hand back work for the host to present? On
Metal, acquire/encode/commit/present are four distinct steps with different
scheduling consequences. The API can stay abstract; the ownership cannot.

### 3.10 Errors

No call in the draft had failure semantics. `create` can fail; so can `tick`,
`resize` and event injection in an invalid state. Exceptions cannot cross the
boundary, and silently ignoring failure produces embedding bugs that are
extremely hard to localise.

State: whether functions return a status enum, whether errors are sticky,
whether a failed call leaves the engine usable, and how the host retrieves
diagnostics.

### 3.11 Extension rules

`structSize` is the mechanism; the rules are the contract. What happens when
`structSize` is *smaller* than the current struct — are trailing fields ignored?
May function pointers be null, and is null "unavailable" or "contract violation"?
Must unknown enum values be tolerated?

The audio provider ABI answers some of this; this one has more lifetime- and
ownership-sensitive fields, so the answers need restating rather than inheriting.

### 3.12 Handle and callback lifetime

`user` carries host state, but nothing guarantees when the engine stops calling
through it. **`destroy` must guarantee that no callback is in flight and none
will be issued afterwards**, or every host races its own teardown.

### 3.13 Surface attach and detach

The draft assumed a surface exists for the whole life of `EngineHandle`. Real
hosts create the engine before the final surface exists, replace a view, move a
surface between windows, or drop the surface while staying alive.

**Surface attachment is an independently managed resource**, not a property of
creation. It needs explicit attach/detach.

### 3.14 Configuration versus runtime state

Which of `EngineConfigV1` is immutable after creation? Size and DPI are runtime
state now that the host owns the surface — the draft had them in both places.

## 4. Three state machines, not one

The draft drew one diagram and conflated three independent things. A host can
stay alive while its surface is destroyed and reattached later; a GPU can be lost
without the app suspending.

```
APPLICATION          created ──► running ──► suspended ──► running
                        │            │            │
                        └────────────┴────────────┴──► destroyed

SURFACE              detached ⇄ attached          (independent of the above)

GRAPHICS             available ⇄ lost ──► recreating ──► available
```

Two transitions the draft omitted, both real:

* **`created → destroyed`** — host setup fails after `create` and before the
  first `tick`.
* **`suspended → destroyed`** — an iOS app backgrounded and then killed without
  ever returning to the foreground. **This is the most common way an iPad app
  ends**, and the draft's model could not express it.

### 4.1 Device-lost recovery is a protocol, not a transition

An arrow from `lost` back to `available` is not a design. Unanswered: who
recreates the device; who recreates swapchain/presentation state; when handles
become valid again; what happens to work submitted while unavailable.

The renderer is the plausible owner, but the **asset system must participate** —
device loss invalidates derived GPU state, which forces the question *"which of
our resources can be recreated from cooked bytes, and which are derived state we
would have to rebuild?"* That question is worth being made to answer, and it is
the reason E3 is larger than it looks. See §8's note on sequencing.

## 5. The ABI sketch — provisional

> **This is a sketch, not a preview of what E4 freezes.** It predates the work in
> E1–E3 that would inform its shape, and every contract in §3 can change it.
> §9 says a frozen ABI is forever; nothing here is frozen.

```c
/* What the HOST provides. */
typedef struct EngineEmbedderV1 {
    uint32_t structSize;
    void*    user;                    /* lifetime per §3.12 */
    /* Surface is attached separately (§3.13), not queried (§3.14). */
} EngineEmbedderV1;

/* What the HOST calls. Serialized, non-reentrant (§3.3). */
EngineResult engineEmbedderCreate (const EngineEmbedderV1*, const EngineConfigV1*, EngineHandle* out);
EngineResult engineEmbedderTick   (EngineHandle, double dtSeconds);   /* §3.1, §3.2 — never blocks */
EngineResult engineEmbedderAttachSurface(EngineHandle, const EngineSurfaceV1*);
EngineResult engineEmbedderDetachSurface(EngineHandle);
EngineResult engineEmbedderResize (EngineHandle, uint32_t pxW, uint32_t pxH, float dpiScale); /* §3.8 */
EngineResult engineEmbedderLifecycle(EngineHandle, EngineAppState);   /* §4 — app state only */
EngineResult engineEmbedderGraphics (EngineHandle, EngineGpuState);   /* §4 — separate machine */
EngineResult engineEmbedderEvent  (EngineHandle, const EngineInputEventV1*); /* §3.4 */
void         engineEmbedderDestroy(EngineHandle);                     /* §3.12 */
```

Changes from the draft, each traceable to §3: `nowNanos` gone (§3.5); size is
**pushed, never queried**, removing the dual authority (§3.14); surface is
attach/detach (§3.13); lifecycle and graphics are separate calls (§4); everything
returns a status (§3.10).

`EngineSurfaceV1` deliberately unspecified here — a typed struct naming which
platform object is valid, and who may interpret it, is better than a bare `void*`
that smuggles backend concepts through a "stable" ABI. That typing is E2's
output, not a guess now.

## 6. What this is NOT

| Flutter embedder has | us |
|---|---|
| surface, resize, DPI | ✅ |
| event injection | ✅ |
| lifecycle / suspend / resume | ✅ |
| who drives the frame | ✅ — the hard part |
| platform channels | ❌ the Kit ABI is that |
| external texture registry | ⚠️ later, if a host asks |
| multiple task runners | ❌ we have `engine::jobs` |

## 7. Falsifiability

`provider-abi.md` §3 rule 1: the default implementation must go through the seam,
or it decays into decoration the way `IRenderPipeline` did.

1. **`engine_player` moves onto the ABI** — and the migration must be real, not a
   wrapper preserving the old assumptions. The test is behavioural: the player
   **creates the platform, owns the loop, polls events itself, pushes resize and
   lifecycle, and calls one non-blocking tick.** If it still calls into runtime
   internals, the inversion is theatre and the ABI is decoration with a new name.

2. **A second embedder.** The headless one is necessary and **not sufficient** —
   it has no surface, so it exercises none of §3.6, §3.8, §3.9, §3.13, or the
   graphics state machine, and it can conform by no-opping most of the contract.

   So: **an offscreen rendering host** as the surface-exercising consumer. It
   renders to a host-owned texture with no window, which reaches attach/detach,
   resize, DPI and present ownership without needing an iPad. The headless one
   still earns its place — it proves the engine runs with no graphics at all —
   but the two prove different halves and neither substitutes for the other.

**Not claimed:** an iPad embedder is the motivating consumer, not a deliverable.

## 8. Phases

| # | Phase | Exit criterion |
|---|---|---|
| **E1** | **Stop blocking, stop pumping.** Move `pollEvents` (`:23`) and the minimise loop (`:31`) into the host. **Requires answering §3.6 first** — deleting the wait removes the current answer for 0×0. | `frameBegin` has no `while` and no `pollEvents`. Both hosts still work. |
| **E2** | **Accept a surface** instead of creating one; produce the typed `EngineSurfaceV1` (§3.13). | An `IPlatform` can wrap a window the host already made. |
| **E3** | **Lifecycle + graphics availability** (§4) and a device-lost recovery protocol. **BLOCKED on open decision 3** — see below. | Suspend/resume and a synthetic device-loss both survive. |
| **E4** | **The C ABI**, with every contract in §3 written down and frozen. `engine_player` moves onto it per §7.1. | Offset pinning; the player uses no runtime internals. |
| **E5** | **Two second consumers**: headless, and the offscreen rendering host (§7.2). | A CI lane with no window backend; another that attaches a surface and presents. |

**E3 is not shovel-ready and the draft implied it was.** Its exit criterion is
concrete but open decision 3 — who owns device-lost recovery — sits underneath
it unanswered, and §4.1 shows the answer reaches into the asset system. Either
decision 3 is settled first, or E3 is scheduled as blocked. `rhi-design.md` and
`renderer-program.md` both flag their own blocked dependencies this way.

E1 and E3 are worth doing even if the ABI is never written: E1 is a latent bug,
E3 a correctness gap.

## 9. Costs

| | |
|---|---|
| E1–E2 | small; mostly moving existing code |
| E3 | real — device-lost forces the resource-recreation audit (§4.1) |
| E4 | **a frozen ABI is forever**, and §3 is thirteen contracts to get right first |
| E5 | small, and it is what keeps the rest honest |
| **standing tax** | a C ABI slows feature velocity (`provider-abi.md` §5); this is a fourth seam |

The tax is the argument against. It is worth paying because the alternative is
that the engine can only be hosted by C++ compiled in this repo — which
forecloses every platform we do not port ourselves, including any future one.

## 10. Open decisions

1. **Does the ABI supersede `IPlatform`, or wrap it?** Wrapping is cheaper and
   keeps three working implementations — but `IPlatform` was designed around an
   **engine-controlled** platform, and a facade over engine-owned semantics can
   say "the host is in charge" while the layer beneath still behaves as though
   the engine is. So: wrap, **but only after E1 and E2** have removed the
   ownership assumptions. Wrapping first would freeze them into the C layer.
2. **Does the engine keep a default run loop?** A convenience
   `engineEmbedderRunLoop()` is how these APIs rot — the convenience path becomes
   the tested one. Recommend no.
3. **Who owns device-lost recovery** — embedder, renderer, or asset system?
   **Unanswered, and E3 depends on it** (§4.1, §8).
4. **Does `tick` return scheduling information?** Returning `void` gives the host
   no way to learn "nothing was rendered", "another frame is needed", or "the
   renderer is unavailable". A retained UI host pays battery for frames it did
   not need to request. Worth deciding deliberately rather than by omission.

5. **Who owns the OS windows — C++ or the UI toolkit?** *(Raised 2026-08-30 by
   the planned Rust/egui editor. Recorded HERE rather than with the editor work,
   because it is an embedder decision that a UI rewrite would otherwise settle by
   accident and permanently.)*

   | | A — C++ owns windows | B — the toolkit owns them |
   |---|---|---|
   | who calls `glfwCreateWindow`/`SDL_CreateWindow` | `IPlatform`, as today | winit, or the toolkit's own backend |
   | `wsi::WindowHandle` is | `GLFWwindow*` / `SDL_Window*` | a handle no C++ TU can implement against |
   | the runtime's input stack | unchanged | **loses its source** — §2.2's inversion, forced |
   | rollout | incremental; new panels beside old ones | all-or-nothing |

   **Recommend A**, and the reason is scoping rather than merit. `wsi::` is used
   by `runtime/input/input_system.h` for seven operations — `installInputSink`,
   `pollKeyboard`, `pollMouseButtons`, `cursorPos`, `feedNativeEvent`,
   `isFocused` — so B does not merely change the editor: it removes the engine
   input stack's source and *forces* the event-push inversion in §2.2. That is
   E1 and E4 arriving as a side effect of a UI rewrite, which is the wrong order
   and the wrong review.

   A is also what keeps the rewrite incremental: a toolkit producing draw data
   for the existing renderer can land one panel at a time.

   **B is the honest long-term shape** — it is what an embedder-owned window
   means — and this plan is how to reach it deliberately. Under B, `wsi::` would
   need window creation it deliberately does not have today, and the handle type
   would have to become something the host supplies rather than something a C++
   TU dereferences.

   Related and separate: **multi-viewport is not inherited either way.** Detached
   panels as real OS windows come from the UI toolkit's own platform backend
   (`imgui_impl_glfw.cpp` today), never from `wsi::`, which has no window
   creation. A new toolkit brings its own viewport model, and that is a port
   rather than a free inheritance.
