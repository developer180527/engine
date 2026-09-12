---
status: plan
---
# A dedicated 3D filmmaking application on this engine

A future-direction note, written 2026-09-11. **Nothing here is built, and nothing
here should be started yet** — see *When*, at the end. It records why the idea is
better-founded than it first looks, what it would take, and in what order, so the
decision can be made on evidence when the time comes.

The question it answers: Unreal and Blender both carry filmmaking toolsets, and
both have become monoliths that do everything at once. Could this engine become a
focused filmmaking tool instead?

**Short answer: yes, and it is unusually well placed to — but as a second product
on the same SDK, not as a fork, and not before the renderer work that it depends
on most has landed.**

---

## 1. Not a fork

The engine's own architecture already argues against forking.
`src/runtime/docs/info.md` states it about the window seam: *"The editor is a
consumer of the SDK, not a layer of it."* The frozen ABI, the three-tier module
model (engine / Kits / game) and the four extension tiers (Plugin, Kit, Add-on,
Provider) exist precisely so one engine can carry more than one product.

A fork means two engines diverging from the first commit: every renderer fix,
every physics bug, every allocator change landed twice or not at all. The right
shape is a **second application that links `engine_runtime`**, the way the game
host and the editor already do.

The rule that follows:

> Anything filmmaking needs that games also benefit from goes into the **engine**.
> Only film-specific workflow and UX lives in the **film product**.

The colour pipeline is the clearest example: film cannot ship without it, and
games need it too. It is designed separately in
[`../colour-pipeline.md`](../colour-pipeline.md).

---

## 2. Why this engine fits: the determinism work is a take system

This is the part that makes the idea worth recording rather than generic.

In virtual production a **take** is: perform something live, keep it, reproduce it
exactly later — re-render at final quality, try a different lens on the same
performance, compare take 3 against take 7. That is, word for word, the invariant
the `determinism-gate` branch established in September 2026:

> same initial state + same commands ⇒ same result

| Filmmaking need | What already exists |
|---|---|
| Record a take | `EngineRuntime::RecordedTick {tick, actionHash, intents, cmds}` — a take at tick granularity (`src/runtime/runtime.h`) |
| Re-shoot a take against a changed scene | The **intent** layer (`src/runtime/sim_intent.h`) records what was *asked*, not what was *derived*, so logic can be re-run rather than replayed — the replay/rollback distinction the header states |
| Render offline at any speed and match the live take | The A/B comparison (1 vs 2 frames per tick) in `tests/determinism_gate_test.cpp` — green on all 14 tier/comparison pairs, physics included |
| Authored cameras separate from the simulated world | The presentation split: `CameraLook` is `SimExempt` (`src/components/camera_look.h`) |
| Batch bake on a farm | The headless target, `NullRenderer`, and the zero-bgfx server build |
| An offline renderer | `IRenderer` (`src/render/renderer_interface.h`) — a path tracer is a second implementation |
| Heavy assets | The cooker, DDC, residency budgets and eviction |
| Character animation | ozz, with the advance/sample split (`src/systems/animator_system.h`) |
| Risky, heavy integrations | The Add-on tier (out of process) |

Unreal's Take Recorder and Sequencer work against non-determinism in live physics
and gameplay, and resolve it by baking. This engine was built around determinism —
a real differentiator, not a marketing one.

**Stated limit, carried from the gate's own header:** it measures
*ECS-observable* determinism. `JPH::PhysicsSystem` internals and `lua_State` are
not hashed, so a green gate does not mean bit-identical simulation. Film would also
need **render** determinism — a separate property that nothing measures yet.

---

## 3. What is missing, largest first

**1. Final-pixel rendering — by far the largest gap.** bgfx is a real-time raster
abstraction. `docs/rhi/evidence-bgfx.md` names its semantic floor (no bindless,
conservative barriers), and bgfx has no hardware ray tracing, which is why
`docs/rhi/design-raytracing.md` is a design document and not code. Film needs
motion blur, depth of field, global illumination, and in practice path tracing.
Realistic route: an offline renderer as a **Provider** behind `IRenderer`, built on
an existing kernel (Embree, Apache-2.0 — *recalled, verify licence before use*)
rather than from scratch. Multi-month on its own.

**2. Colour management.** Not merely absent — the engine currently shades in an
undefined colour space. Measured and designed in
[`../colour-pipeline.md`](../colour-pipeline.md). Belongs in the engine.

**3. A sequencer / timeline.** Tracks, clips, keys, shots, sequences. Does not
exist. It would sit naturally on the command record: a timeline track is a
scheduled stream of `SimCommand`s.

**4. Film-rate time.** `kSimDt` is 1/60 (`src/runtime/runtime_sim.cpp`). Film runs
at 24 fps, and 60/24 = 2.5 — not an integer. Either the sim rate becomes
configurable, or it runs at a common multiple such as 120 Hz (5 × 24). Motion blur
additionally needs sub-frame sampling. Small in code, real in design.

**5. USD.** Universal Scene Description is the industry's interchange standard;
without it the tool is an island. Its dependency weight makes it an **Add-on**.
OpenTimelineIO for editorial and OpenEXR for output belong alongside it.

**6. A physical camera model** — sensor size, focal length, aperture, focus
distance, shutter angle, rigs (dolly, crane, handheld). The exposure half of this
is shared with the colour pipeline (`colour-pipeline.md` stage B uses EV100).

---

## 4. On monoliths

Unreal and Blender are not monoliths only by accident. Filmmaking spans modelling,
rigging, animation, simulation, lighting, rendering, compositing and editing. A
"dedicated" tool still needs most of that pipeline — or it must **interoperate**
with the tools that have it.

The focused tools that survive — Houdini for procedural and FX, Nuke for
compositing — are excellent at one stage and speak the industry's formats to
everything else. A focused tool that does not interoperate slowly grows the
missing pieces and becomes the monolith it set out to avoid.

So "not a monolith" means **small core + strong interchange + plugins**: USD in,
OpenTimelineIO for editorial, EXR and OCIO out. The same philosophy as the
frozen-ABI tiers, applied to a pipeline instead of a codebase.

---

## 5. The niche to aim at

Not "a film tool" in general. The one the determinism substrate makes this engine
*better* at than the incumbents:

**Deterministic virtual production and previs.** Block out a scene, perform it
live with real physics and characters, keep the takes, then re-render any take at
final quality offline — guaranteed to match what was performed. Animated shorts
and machinima fit the same shape.

---

## 6. How it would be structured

```
            ┌───────────────────────── film product ─────────────────────────┐
            │  sequencer / timeline · take manager · camera tools · render   │
            │  queue · shot/sequence structure · editorial export            │
            └───────────────┬───────────────────────────────┬────────────────┘
                            │ links                         │ loads
  ┌─────────────────────────▼──────────────┐   ┌────────────▼──────────────────┐
  │ engine SDK (shared with games)         │   │ extensions                     │
  │  runtime · ECS · sim + determinism     │   │  USD / OTIO / EXR   (Add-on)   │
  │  command + intent record · cooker/DDC  │   │  offline renderer   (Provider) │
  │  animation · physics · input           │   │  mocap ingest       (Add-on)   │
  │  colour pipeline  ← needed by both     │   └────────────────────────────────┘
  └────────────────────────────────────────┘
```

---

## 7. When, and in what order

**Not now.** The engine's renderer programme — P3 retained scene, the G0a
GPU-driven spike (`docs/plans/renderer-program.md`, `docs/rhi/phases.md`) — is
still in progress, and the film product's single largest dependency is rendering
quality. Building it first means building on sand.

What *can* be done meanwhile is to keep choosing work that serves both products:

1. **Colour pipeline** stages A–C, in the engine — a correctness fix for games,
   mandatory for film. [`../colour-pipeline.md`](../colour-pipeline.md).
2. **Configurable sim rate + sub-frame stepping** — cheap, harmless for games,
   essential for film.
3. **A sequencer as a Plugin in the existing editor** — prove the timeline on the
   current codebase before committing to a separate application.
4. **Takes on top of the `RecordedTick` ring** — mostly UX over what exists.
5. **USD import/export as an Add-on.**
6. **An offline renderer as an `IRenderer` Provider.**

By step 3 it will be clear whether this wants to be its own application or a mode
of the editor — a decision better made on evidence than on the idea.
