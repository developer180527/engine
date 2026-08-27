---
status: decided
covers:
  - src/tools/
  - src/editor/panels/
  - include/engine/addon_protocol.h
---
# The Tool Ecosystem — what the editor is for, and what it is not

**Where a capability lives: inside the editor, or in its own program reached
through a contract.**

This is [`extension-model.md`](extension-model.md) extended past the runtime.
That document answers *how code attaches to the engine*; this one answers *how
TOOLS attach to the pipeline*, and it leans on the tier that document already
defined and did not build:

> | | **Add-on** *(batch v1 built)* |
> |---|---|
> | Linkage | separate **process** |
> | Boundary | IPC |
> | A crash takes down | only itself |
> | For | **tools** |
>
> **The process boundary is affordable exactly where the frame boundary is not.**

> **What exists today.** Fourteen separate tools (§5), an editor that already
> holds the line (§5), and a cooked-asset pipeline that already makes the seat
> rule work (§4). The Add-on protocol now exists for the **batch** direction —
> `include/engine/addon_protocol.h`, spoken by `engine_module_probe` and
> `engine_build` (§5) — and each conversion found a real defect: a forgeable
> output channel in the first, and twelve defects-in-the-shipped-package that
> exited 0 in the second.
> What does NOT exist is **live link**, the direction where a running tool pushes
> into a running editor, and §10 says why that is a separate protocol rather than
> a missing half of this one. Every other tool still speaks its own
> argv-and-stdout contract; §6 says what to do about that and why it is not
> urgent.

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

### It is now a protocol, not a pattern

Step 1 and step 2 of §8 are done. `include/engine/addon_protocol.h` is the v1
**batch** Add-on protocol — the invoke direction only — and `engine_module_probe`
is its first speaker, converted rather than written for the purpose, exactly as
step 2 asks.

Three rules, and the third is the probe's own insight generalised:

1. **stdout and stderr are human channels.** Always. Rewording them is never a
   breaking change.
2. **The result file is the machine channel** — framed, with a line count and
   digest, so a partial write is *detectably* partial.
3. **Exit status says whether the tool ran**, never what it decided. A refusal is
   a successful probe of a bad module, at exit 0, reported as a record.

**The conversion found a real defect, which is what converting an existing tool
is for.** The probe's machine-readable verdicts were on stdout — and the probe
`dlopen`s modules it does not trust, which is the entire reason it is a separate
process. Loaded code can print. A module's static initialiser or its `create`
could write `module 0 ok /x`, or forge the `probe done` terminator, in bytes
indistinguishable from the host's own output. Not noise: **impersonation**, in
both directions — inventing an accept that never happened, or making a run that
died halfway look complete.

`engine_cook_worker` had already written the rule down — *"cookers print freely,
so stdout is not a channel"* — and the probe, whose exposure is strictly worse,
had not applied it. A channel shared with the thing under test is not a channel.
`abi_gate_noisy_stdout` is a fixture that performs the forgery at both moments a
module can run code, and the conformance suite requires the verdicts to survive
it.

### 5.1 The second speaker, and what it cost the protocol

`engine_build` — the editor's fifth job, **export** — is the second Add-on, and
it is where the protocol stopped being a guess.

**It had a worse version of the probe's defect.** Twelve warning sites, each
describing a defect in the *shipped game*: an unreadable cooked scene whose every
object is missing, a scene referencing a mesh that is not there, a mesh
referencing a texture that is not there, two shaders both claiming one name, no
shader providing `standard` so the player silently falls back to compiled-in
shading, material textures resolving to nothing and shipping white.

All twelve went to stderr, and **the process exited 0.** No caller could see
them — not the editor, not CI, not a build farm; only a human reading scrollback,
who has no reason to scroll back because the status said the package was fine.
That is the silently-untextured build again, arriving through the packager instead
of the IPC channel.

They are records now, with a `WARNINGS` count. Two rules came out of it:

- **The tool reports; the caller decides.** A warning does not fail the build by
  itself, because whether a missing texture blocks a ship is policy and a
  packager is not where policy lives. `--strict` is how a caller that wants them
  fatal says so, and CI is expected to pass it.
- **Exit 1 is gone.** It had meant fifteen different things in that one file —
  and the protocol reserves 1 as *unassigned* precisely because it is the exit
  code of every accident. A failed kit build and a segfault in cmake used to be
  the same number.

**Two things the second speaker changed in the protocol itself**, which is the
whole reason to convert rather than invent:

1. **`recordExact` alongside `record`.** v1 sanitised every record value —
   correct for a *message*, dangerous for a *path the caller opens*. Mangle a
   warning and a human reads an odd sentence; mangle a path and the caller opens
   a file that does not exist, turning a precise failure into a silent wrong
   answer. `recordExact` refuses instead, and the caller must handle the refusal.
   The probe never exposed this: it only ever put paths in records as diagnostic
   text, so mangling was harmless there. **One speaker cannot tell you whether a
   protocol generalises.**
2. **`Exit::RanWithFailure` (5).** The probe has one kind of caller — a host that
   reads the result file — so exit 0 with a `refused` record is perfect. A
   packager is also run by people and by CI, where the status is the *only*
   channel, and a packaging step that exits 0 on a defective package is the bug
   being fixed. So 0 and 5 both mean *the tool ran and its result is trustworthy*
   — which is what rule 3 actually protects — and they differ only in the
   verdict. The rejected alternative was making the exit status depend on whether
   `--addon-result` was passed; that lets a tool's contract change shape based on
   how it was called, which is worse.

The same pass found one real bug in `engine_cook_worker` without converting it:
`ERROR` was sanitised and `OUTPUT` was not, and `OUTPUT` carries a path whose stem
comes from the asset filename. A newline is legal in a POSIX filename, so such an
asset produced a body one line longer than the writer counted, the frame failed
its own line-count check, and the parent blamed the worker's write for what was a
filename. It now refuses, naming the file — *not* sanitises, because the parent
**opens** that path.

Two things about how it is verified are worth stating, because they are the
difference between a protocol and a format someone wrote once:

- **The Rust suite reimplements the frame**, digest included, rather than binding
  to the C++. A test that calls the writer's own reader agrees with the writer by
  construction — rename a key, change the seed, and both halves move together and
  stay green. It is also the honest simulation of the real client, since an Add-on
  may be written by anyone in anything.
- **A missing result file is a hard failure, never a fallback to stdout.**
  Otherwise the conversion would be decorative: a broken writer would silently
  revert to the forgeable channel with every test still passing.

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

*Now specified and demonstrated:* `--addon-manifest` on any Add-on prints a
framed self-description. One lesson from building it is worth keeping, because it
generalises past manifests: **a manifest nothing cross-checks is decorative.**
The first test written for it asserted the manifest declared the record key the
suite parses — and mutation testing showed it passed while the key was renamed
out from under nine other tests, because it had only compared one string literal
in the tool against the same literal in the test. It is load-bearing only now
that it is checked in both directions against what a real run actually emits.

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

1. ~~**Write the Add-on protocol**~~ — **done, for the batch direction.**
   `include/engine/addon_protocol.h`: the invoke shape, the framed result, the
   manifest, the exit-code contract. Live link is explicitly not in v1; see §10.
2. ~~**Prove it by converting one tool that already exists.**~~ — **done.**
   `engine_module_probe` speaks it, and the conversion found a forgeable channel
   nobody had noticed (§5).
3. ~~**Convert a second tool**~~ — **done: `engine_build`**, and it is the
   conversion that mattered. See §5.1.

   I named `engine_cook_worker` here first and that was **wrong**. It is not an
   Add-on and should not become one: it is fork/exec'd by assetlib from the same
   build, in lockstep, so version negotiation, a discovery manifest and a
   self-describing vocabulary all answer questions that cannot arise. No human
   runs it; the editor does not drive it; a library does, as an implementation
   detail. Converting would also force assetlib — a standalone project,
   deliberately independent of the engine SDK — to depend on `include/engine/` to
   gain nothing.

   The distinction is worth keeping, because the mechanism is identical and the
   purpose is not: **a worker process is blast containment; an Add-on is
   untrusted extension.** Assimp may `SIGSEGV`, so put it in its own address
   space — that is not the same problem as talking to a tool someone else built.
   Conflating them is how a protocol acquires users who do not need it.
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

- ~~**Does the Add-on protocol carry structured data, or only paths?**~~
  **Answered by scoping, not by choosing.** v1 is the batch direction: argv in, a
  framed result file out, records as `KEY value` lines. Live link is a different
  protocol in a different document, because a persistent connection with
  incremental state shares nothing with a request/response frame. What remains
  open is narrower and better: **the record vocabulary is deliberately unfixed.**
  `CONSUMES` and `PRODUCES` are free-form tokens, and an asset-type ontology is a
  thing to extract from three working Add-ons rather than invent for the first —
  a wrong one here would be copied by everything after it.
- **Who launches whom?** The editor invoking tools is obvious. A tool wanting to
  push into a *running* editor is the live-link direction and needs a discovery
  mechanism the editor does not have.
- **Where do tool manifests live** — in the project, next to the engine, or in a
  user profile? A studio wants the first, a solo developer the third.
- **Does the editor ever bundle a minimal authoring tool** for the "plain
  complete game" floor, and if so how is it stopped from growing into the second
  texture painter this document exists to prevent?
