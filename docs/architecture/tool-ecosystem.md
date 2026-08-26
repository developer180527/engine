---
status: decided
covers:
  - src/tools/
  - src/editor/panels/
---
# The Tool Ecosystem — what the editor is for, and what it is not

**Where a capability lives: inside the editor, or in its own program reached
through a contract.**

This is [`extension-model.md`](extension-model.md) extended past the runtime.
That document answers *how code attaches to the engine*; this one answers *how
TOOLS attach to the pipeline*, and it leans on the tier that document already
defined and did not build:

> | | **Add-on** *(unbuilt)* |
> |---|---|
> | Linkage | separate **process** |
> | Boundary | IPC |
> | A crash takes down | only itself |
> | For | **tools** |
>
> **The process boundary is affordable exactly where the frame boundary is not.**

> **What exists today.** Fourteen separate tools (§5), an editor that already
> holds the line (§5), and a cooked-asset pipeline that already makes the
> seat rule work (§4). What does NOT exist is a formal Add-on protocol — every
> tool today speaks its own argv-and-stdout contract. §6 says what to do about
> that and why it is not urgent.

---

## 0. The failure this prevents

Unity, Unreal and Godot all ship a texture painter, an animation editor, an audio
mixer, a particle designer, a terrain sculptor and a video sequencer. None of
those is *wrong* on its own. The problem is what they add up to, and the
mechanism is worth naming precisely, because "they got bloated" is not a lesson
anyone can act on.

**Nobody models in Unreal.** The big domains were conceded decades ago — rigs and
animation come from Maya or Blender, textures from Substance, FX from Houdini,
audio from Wwise or Reaper. So the monoliths are not monolithic because a
capability *had* to be inside. They grew because the claim "you never have to
leave the editor" is worth more in marketing than the second-best texture painter
in the world costs to maintain.

The result is an editor with two texture workflows, neither excellent, and a
newcomer who cannot tell which one a studio actually uses. That is the clutter.
It comes from **duplicating tools that already exist**, not from ambition.

This document exists so that every future "should this be in the editor?" has an
answer that is not a matter of taste.

---

## 1. The rule

> **Authoring lives outside. Tuning and viewing live inside.**

Three tests, in the order they are useful.

### Test 1 — the iteration loop

> *Can you judge the result without the game running?*

- A texture is judged by looking at it. **Outside.**
- Whether a footstep fires at the right moment while sprinting downhill can only
  be judged with the game running, the animation blending and the audio mixing.
  **Inside.**

This is the load-bearing test, and it is not about how specialised a tool is. It
is about whether the *other systems* have to be live for the answer to exist.

### Test 2 — source of truth

> *Exactly one tool may own each datum.*

If the editor can edit animation curves and Blender can too, there are two
sources of truth, and no amount of merge tooling makes that recoverable. The
editor may **reference** and **override**; it may not **author** what another tool
owns.

This is the rule that decides whether a pipeline is still working in year three.

### Test 3 — authoring or parameterisation?

Unreal's one genuinely excellent boundary: a material **graph** versus a material
**instance**.

- The graph creates and destroys structure. That is authoring.
- The instance sets exposed numbers on a graph someone else fixed. That is
  tuning — and it must be inside, because you tune against your own lighting.

When a domain feels like it belongs in both places, this is usually why: it has
an authoring half and a parameter half, and only the second one was ever wanted
inside.

---

## 2. Where each domain lands

| Domain | Outside — authoring | Inside — tuning and viewing |
|---|---|---|
| Code | the editor you already use: VS Code, Rider, Vim | binding scripts to entities, hot reload, live inspection, error surfacing |
| Textures | painting, masks, channel packing | which texture fills a material slot; exposed parameters; budget and format target |
| Meshes | modelling, UVs, LOD authoring | placement, LOD thresholds, collision assignment |
| Animation | keyframes, rigs, skinning | state machines, blend weights, how it answers input |
| Audio | waveform editing, mastering, bank authoring | spatial placement, in-scene mix, trigger wiring |
| VFX | node graphs, simulation | emitter placement, budgets, LOD |
| Scene | — | **the editor authors this.** Composition is its own data |

Every right-hand column shares one property: it only means anything with the
other systems live. That is not a coincidence — it is Test 1 restated.

Note the last row. The editor is not *only* a viewer. It **authors the
composition** — which entity exists, where, with what components and what
parameters. That is the editor's own domain and no external tool owns it.

---

## 3. What the editor owns

> **The editor owns the composed, running game. Everything upstream of that is
> somebody else's tool, reached through a contract.**

Concretely, five jobs and no sixth:

1. **Compose** — the scene: entities, hierarchy, components, references.
2. **Configure** — project settings, input bindings, budgets, target platforms.
3. **Tune** — parameters on assets someone else authored.
4. **Test** — play, profile, inspect, A/B.
5. **Export** — cook, package, ship.

Anything that is not one of those five is a candidate for its own program. That
is the whole test, and it is short on purpose: a rule nobody can recite does not
get applied when it matters.

---

## 4. The seat rule

> **Everyone has the editor. Nobody needs every tool.**

A level designer has the editor. An animator has the editor *and* the animation
tool. A sound designer has the editor *and* the audio tool. This is how studios
actually work, and not only for licence cost — a specialist tool is a skill, not
just an install.

It forces one hard architectural requirement:

> **Opening, viewing, and building a project must never require an authoring tool
> to be installed.**

**This is already true here, and it was not free.** The DDC keys a cooked artifact
on the hash of its inputs and shares that store across machines; the cooked bytes
travel and the source need not be present at all. `ENGINE_WITH_SOURCE_IMPORTERS=OFF`
proves the extreme case — a 6.4 MB player with no Assimp compiled or linked. The
same split serves artists: the animator cooks, everyone else consumes.

### The nuance that is easy to get wrong

A level designer without the animation tool still has to **preview animations**.
One without the audio tool still has to **hear the mix**. Take that away and you
have something technically clean that nobody can work in.

So each domain needs a **viewer** in the editor. A viewer is not an authoring
tool, and the difference is exactly Test 2: a viewer never becomes a second
source of truth.

> **Viewers and tuners in. Editors out.**

---

## 5. What exists today

### The tools are already separate programs

| Tool | Job |
|---|---|
| `engine_editor` | compose, configure, tune, test, export |
| `engine_host` | run a project headlessly with a dev module attached |
| `engine_player` | the shipping runtime |
| `engine_cook` | cook a project's assets |
| `engine_cook_worker` | cook ONE asset, out of process |
| `engine_build` | package a distributable |
| `engine_project` | scaffold a new project |
| `engine_module_probe` | run the module load gauntlet and report per module |
| `scene_resave` | load and re-save a scene (format migration) |
| `input_ab` | A/B input latency measurement |
| `profiler_frames` | frame capture inspection |
| `engine_doctor` | the docs and maturity contract |
| `check_std_includes` | the portability lint |
| `gen_fuzz_scene`, `gen_stress_scene` | corpus generators |

That is not a plan. That is `ls src/tools/` plus `ls scripts/`. **The ecosystem
already exists**; what it lacks is a stated contract.

### The editor already holds the line

An audit of all fifteen panels against §1:

| Panel | Kind |
|---|---|
| `game_view` | viewer — the composed running game, the editor's core |
| `hierarchy` | **authoring** — of scene composition, which the editor owns |
| `inspector` | tuning |
| `asset_browser` | viewer |
| `input_bindings` | tuning |
| `project_settings` | configuration |
| `profiler` | viewer |
| `console`, `internal_console` | viewers |
| `plugins` | viewer + configuration |
| `script_viewer` | **viewer** |
| `terminal` | orchestration |
| `menu_bar` | chrome |

Two of those deserve attention.

**`script_viewer`, not `script_editor`.** Someone already made the correct call
and named it honestly. Code is authored in a real code editor; the engine shows
you what it loaded.

**`terminal_panel` is the orchestration seam, already.** It runs commands from
inside the editor. Every "the editor invokes a tool" story below is a formalised
version of what that panel does informally today.

### The contract pattern is already in use

Two tools already demonstrate the shape an Add-on protocol would formalise, and
both did it for the same reason.

`engine_cook_worker` takes `(source, output, result, budget)` on argv and reports
through a **framed result file** — magic, version, `RESULT ok|skip|fail`, an
`ERROR` line, `OUTPUT` lines, and a trailer with a line count and digest so a
truncated result is detectable rather than read as a clean success.

`engine_module_probe` prints one line per module on stdout and a terminator, with
the human-facing explanation on stderr, and exits 0 whenever *the probe ran* —
refusals included, because a refusal is a successful probe of a bad module and
collapsing it into a non-zero exit makes it indistinguishable from a crash.

Its header states the principle better than a protocol document would:

> *"A command line is a boundary that already has to be stable, the same way
> `engine_cook_worker`'s is, so the Rust suite drives this and no shim exists
> purely to be tested."*

That is the pattern: **a tool's CLI is an ABI.** It is versioned, it is
documented, it is tested from outside, and it is stable because something other
than a human depends on it.

---

## 6. What orchestration requires

Five requirements. They are what separates an ecosystem from a folder of
scripts, and most studio pipelines fail on the first and the last.

**1. Headless invocation, non-negotiable.** Every tool must be drivable with no
human clicking. A tool that can only be operated by a person cannot be
orchestrated by the editor, and cannot be run in CI either — which means its
behaviour is unverified. Everything in the table above already satisfies this.

**2. A manifest per tool** — what it consumes, what it produces, how to invoke
it, which contract version it speaks. `EngineContractDecl` and the frozen ABI
tables are this idea applied to modules; a tool manifest is the same idea applied
to processes.

**3. The DDC is the substrate. Do not build a second one.** A tool becomes a
cooker or a pre-cook step. Its output is content-addressed on its inputs, its
version and its settings fingerprint; cross-machine sharing, staleness and GC all
come for free. This is the piece pipelines usually get wrong, and it is the piece
that already works.

**4. Staleness must be answerable without the tool installed.** The editor has to
say *"this asset's source changed and needs re-cooking with the animation tool"*
on a machine that does not have the animation tool. That is a registry question,
not a tool question, and `AssetRegistry` already answers it.

**5. Deep links.** *"Open this asset in its authoring tool, at this frame, on this
layer."* This is the glue that makes several programs feel like one system.
Without it, a multi-app pipeline feels like a punishment rather than a design —
and this is the requirement most likely to be skipped, because nothing fails
without it. It just quietly makes everyone slower.

---

## 7. The costs, stated

The monoliths won for reasons. Anyone adopting this model should adopt the
mitigations with it.

| Cost | Why it bites | Mitigation |
|---|---|---|
| **Iteration latency** | alt-tab, export, re-import, look — far slower than editing in place. The single biggest reason monoliths exist. | **Live link**: the tool streams changes into a running engine. Substance, Maya and Wwise all do this. Not a luxury in this model; the thing that makes it survivable. Defer until one loop actually hurts, then build it there. |
| **Version skew** | N tools × M versions is where pipelines actually break. | Formal contracts, not conventions. Frozen layouts, `structSize`, offset asserts, `>=` accepts — the discipline already applied to the module ABI, applied to tool protocols. |
| **Onboarding** | a solo developer now installs and learns several programs. | **The editor must be sufficient for a complete, if plain, game.** Placeholder materials, imported meshes, basic triggers. Specialist tools raise the ceiling; they must never raise the floor. |
| **The last mile** | some edits are genuinely cross-cutting — a material that looks wrong only under one level's lighting. | This is what the tuning tier is *for*. If a cross-cutting fix requires leaving the editor, the parameter should have been exposed. |
| **The tooling tax** | studios doing this end up with a tools team. | Do not build tools. Build the **seam**, and let tools be optional. §8. |

That last row is the real risk for a one-person engine, and it is why this
document ends where it does.

---

## 8. What to do, in order

1. **Write the Add-on protocol** that [`extension-model.md`](extension-model.md)
   already specifies and does not implement: the IPC shape, the manifest, the
   lifecycle, the failure modes. This is the one piece with no substitute.
2. **Prove it by converting one tool that already exists.** `engine_cook_worker`
   and `engine_module_probe` are both nearly Add-ons with bespoke contracts.
   Converting one is a real test of the protocol; inventing a new tool to
   demonstrate it is not.
3. **Keep the editor's domain panels viewers.** An animation panel that plays and
   blends but cannot keyframe is not an unfinished feature. It is the design.
4. **Adopt, do not author.** Blender, Substance, Reaper, Audacity and VS Code
   already exist and are better than anything this project will write. The
   differentiator is the seam.
5. **Live link last**, and only where a measured loop hurts.

---

## 9. The rules, in one place

- **Authoring outside. Tuning and viewing inside.**
- **The editor owns the composed, running game** — compose, configure, tune,
  test, export. There is no sixth job.
- **Exactly one tool owns each datum.** The editor references and overrides; it
  does not author what another tool owns.
- **Opening, viewing and building a project never requires an authoring tool.**
- **Every domain gets a viewer in the editor.** A viewer is not an editor.
- **A tool's CLI is an ABI** — versioned, documented, tested from outside.
- **Every tool is headlessly drivable.** If only a human can run it, it is
  unverified.
- **The DDC is the orchestration substrate.** Do not build a second one.
- **Staleness is a registry question**, answerable without the tool present.
- **The editor is sufficient for a plain complete game.** Specialist tools raise
  the ceiling, never the floor.

---

## 10. Open questions

Recorded rather than answered, because guessing at them would be worse than
leaving them visible.

- **Does the Add-on protocol carry structured data, or only paths?**
  `engine_cook_worker` passes paths and a framed result file, which is simple and
  crash-safe. A live-link tool needs a stream. Those may be two protocols rather
  than one, and pretending otherwise would produce a protocol that serves neither.
- **Who launches whom?** The editor invoking tools is obvious. A tool wanting to
  push into a *running* editor is the live-link direction and needs a discovery
  mechanism the editor does not have.
- **Where do tool manifests live** — in the project, next to the engine, or in a
  user profile? A studio wants the first, a solo developer the third.
- **Does the editor ever bundle a minimal authoring tool** for the "plain
  complete game" floor, and if so how is it stopped from growing into the second
  texture painter this document exists to prevent?
