---
status: plan
covers:
  - src/plugins/
  - src/tools/engine_build.cpp
  - src/project/project_context.h
---
# Provider selection — declared once, resolved before the game runs

> **Status: plan, 2026-11-26.** Built so far: the registry (WO-076), manifests and
> platform tables (WO-085), and the resolver with `engine_build --platform` (WO-086).
> It turns the research in
> [`provider-selection-research.md`](provider-selection-research.md) (how Unreal,
> Unity, Godot, O3DE, CryEngine, Bevy and the graphics libraries do it) into
> rules and work orders. It does not change the shape of a provider's interface;
> that is [`provider-abi.md`](../architecture/provider-abi.md). It decides
> **how a provider is declared, chosen, built and checked.** Decision: DR-0037.

## 1. What the other engines taught us

Every engine studied does the same basic thing: an implementation is a module
registered under a name, a setting picks one, and one build switch turns DLLs
(development) into static linking (shipping). Their problems are all in **when
and where a bad choice is caught.** Each lesson below becomes one of our rules.

| # | What we saw | Where | Our rule |
|---|---|---|---|
| 1 | A missing or misspelled provider is found when the game runs: a dummy fallback, an ignored flag, a fatal error at startup | Godot (Dummy server), O3DE (`--rhi` ignored with a warning), CryEngine (`CryFatalError`), Unreal (ini module names) | **R1** Resolve before the game runs |
| 2 | An unsupported backend is quietly turned off or stubbed at configure time, so the build passes | CryEngine (`RENDERER_*` options turn off without the SDK), O3DE (empty `ly_create_alias`, `*_unsupported` files) | **R2** Unavailable is visible; chosen-and-unavailable is a refusal |
| 3 | Platform lists exist but nothing that decides reads them | O3DE `gem.json` `platforms` (metadata only); Unreal `.uplugin` lists unevenly enforced | **R3** One manifest, read by everything that decides |
| 4 | Static self-registration drops objects nobody references | (known C++ behaviour; WO-076 already notes it) | **R4** Registration is an explicit, generated list |
| 5 | Providers reach each other and a global environment | CryEngine `gEnv`; Bevy's implicit `ready`/`finish` plugin dependencies | **R5** Providers depend on engine services, never on each other |
| 6 | Same interface, different behaviour | Unreal's PhysX → Chaos; Godot's documented Jolt differences; Unity's URP/HDRP content split | **R6** A behaviour contract per kind, and a written list of differences |
| 7 | The shipping configuration is not what the tests run | O3DE (`LY_MONOLITHIC_GAME` turns off tests) | **R7** Test the build that ships |
| 8 | Cost is managed by crossing rarely, not by removing the call | bgfx `submit(Frame*)`, Unreal RHI command lists, Unity draw lists | already `provider-abi.md` §1; measured by its A/B rule |

The one precedent for refusing at build time is sokol_gfx's `#error`. We go
further than any engine studied on R1 to R3; that is deliberate, because those
are the points every one of them documents as pain.

## 2. Three layers, one place each can fail

Choosing "a renderer" is really three choices. Keeping them apart is what stops
a D3D12-only part from tangling with the rest of the platform.

| Layer | Example | Chosen by | Fails at |
|---|---|---|---|
| **Provider** | renderer `aaa` / `web` / `none`; physics `jolt` / `none` | `project.json`, per target (§4) | `engine_build`, or dev startup (§5). Never at a shipped game's startup. |
| **Graphics backend** (inside librhi) | D3D12, Vulkan, Metal | compiled per platform; an ordered list in `project.json` | startup: the first in the list that creates a device. The list is the only fallback. |
| **Device capability** | bindless, mesh shaders | probed at startup | startup: refused by name, or the provider's declared fallback (`engine-architecture.md` §6) |

Graphics APIs are never providers. One renderer covers every platform librhi
supports; a provider per API makes sense only for a renderer that genuinely
exists on one platform, such as a console's.

## 3. The provider manifest (R2, R3)

Each provider has one file beside its source, `provider.json`. CMake, the
resolver, `engine_build`, the editor and `--list-providers` all read it, and
nothing else describes the provider.

```json
{
  "kind": "physics",
  "name": "jolt",
  "contract": "engine.physics.v1",
  "platforms": ["windows", "linux", "macos", "ios", "android", "web"],
  "needs": { "platform": ["threads"] },
  "requires": ["engine.jobs.v1", "engine.debug_draw.v1"],
  "capabilities": ["rigid_bodies", "character", "determinism.same_binary"],
  "differences": "docs/differences.md"
}
```

- **`platforms` is an allow list.** There are no deny lists: a new platform is
  supported only once someone says so.
- **`needs` names platform facts,** read from one table per platform
  (`cmake/platforms/<name>.json`: `display`, `threads`, `dlopen`, `gpu_apis`, …).
  This is O3DE's PAL traits as data, in one place.
- **`sdk`** (for Wwise, FMOD, a console SDK) names what must be found, per build
  configuration, because middleware ships separate Debug, Profile and Release
  libraries.
- **`requires` names services, never providers** (R5).
- **One CMake call per provider,** `engine_provider(<dir> SOURCES …)`. It reads
  the manifest and the target's platform table. If the platform matches and the
  SDK is found, it builds the provider: a shared library in a development tree
  (WO-077), a static library otherwise. If not, it records why not, for the
  resolver and the listing. **Not built is normal; chosen and not built is the
  error** (§5).

## 4. Choosing per target

A **target** is a platform plus a flavour (`client`, `server`). A plain string
means every target; an object picks per target.

```json
"providers": {
  "renderer":  { "default": "aaa", "web": "web", "server": "none" },
  "physics":   "jolt",
  "audio":     { "default": "miniaudio", "server": "none" },
  "scripting": "lua"
},
"rhi": { "windows": ["d3d12", "vulkan"], "linux": ["vulkan"] }
```

- **The most specific key wins:** `<platform>.<flavour>`, then `<flavour>`, then
  `<platform>`, then `default`.
- **An unknown key is refused** (`widnows`), as an unknown name is.
- **Every kind's null provider is called `none`** and is chosen by name, for
  dedicated servers and the headless farm. It is never a fallback.
- **`rhi`** is optional. Leaving it out takes librhi's per-platform order.
  Apple platforms have Metal only, so they take no list.
- Today's files keep working: `"physics": "jolt"` is a plain string, and a
  project with no `"renderer"` gets the engine default.

## 5. The resolver: one function, three callers (R1)

```
resolve(project, target, manifests, platform table) -> selection | refusals
```

The function is the same in every caller:
- **`engine_build --platform <p> [--flavour <f>]`**: before anything compiles;
- **development startup** (editor, `engine_host`, a dev `engine_player`), for the
  machine it runs on;
- **`engine providers --platform <p>`**, which prints the result as a table:
  per kind, what was chosen, which key chose it, and every other provider with
  the reason it was not usable.

It checks, in this order, and each refusal names the thing and what was
available:

1. the kind exists, and every target key is a platform or flavour this engine knows;
2. the name is a provider some manifest declares;
3. its `platforms` include the target's platform;
4. the target's platform table has every fact in `needs`;
5. its `sdk` was found for this build configuration;
6. every service in `requires` comes from core or another chosen provider;
7. exactly one provider fills each kind.

**What it produces** for a shipped game:
- **`providers.lock.json`** in `dist/`: each kind's provider, its contract
  version and its source revision. The crash reporter prints it, and the folder
  check (WO-066) compares the binary against it.
- **One generated source file** that calls each chosen provider's register
  function (R4). The player is linked with exactly those providers.
  The shipped player contains no resolver, manifest reader or loader (DR-0032).

## 6. Keeping providers apart (R5)

- **A provider includes only core and service headers,** and links no other
  provider's target. The CMake call refuses such a link, and an audit rule
  (`PROV-01`) checks the includes.
- **Anything crossing providers goes through a service the engine owns:**
  - physics debug shapes go to the engine's debug-draw service (`src/core/debug_draw.h`;
    it must stop packing colours in bgfx's layout and including `bx`);
  - the renderer gets its surface from the host (`platform-embedder.md`);
  - audio gets the listener's pose from the world, not from the renderer.
- **No global environment.** A provider's create call gets one context struct:
  host services, log, allocator tags, jobs, and the surface if it has one.
  `provider-abi.md` rule 9.

## 7. Behaviour, not just an interface (R6)

- **The conformance suite per kind** (audio has one) runs against every
  provider, in each link mode it builds in.
- **Every non-reference provider writes its differences down**, named by its
  manifest's `differences` field: the documented ways it behaves unlike the
  kind's reference, as Godot does for Jolt. A difference the suite tolerates
  is written there, not discovered by a game.
- **Renderers share one content contract** (`renderer-contract.md`): materials,
  lights and cameras mean the same in `aaa` and `web`. Content that uses one
  renderer's extension declares its fallback. This is the lesson of Unity's
  URP/HDRP split, where assets for one did not work in the other.

## 8. Testing what ships (R7)

- **CI builds the shipped player** (static providers, `ENGINE_SHIPPING`) for each
  platform leg, for the default and `server` selections of a sample. It runs
  the conformance suites and a headless smoke scene on that binary.
- **The symbol check covers providers:** the shipped binary contains the chosen
  providers and no others.
- **The home server keeps the A/B number** (`provider-abi.md` §3): the default
  provider linked direct against the same provider through its table.
  C function tables cannot be devirtualised by LTO, so this is the only honest
  answer to what the seam costs.

## 9. Development time

- **The same manifests and resolver run at startup,** so dev and ship cannot
  disagree about what is allowed.
- **Providers load as shared libraries by name** (WO-077). A change of provider
  happens with the simulation stopped, or on restart; never mid-play.
- **A command-line override** (`--provider physics=none`) applies to one run and
  is never written back. Unreal shows why: its RHI flags land in a per-user
  ini, and the project then runs differently on one machine with no visible cause.

## 10. What we deliberately do not copy

- **Runtime fallback chains** (Godot's `fallback_to_*`, bgfx's next backend).
  The one fallback is librhi's declared, ordered list (§2).
- **Deny lists.** Allow lists only.
- **Hidden per-user state.** Overrides are per run.
- **A provider per graphics API.**
- **Hot swapping mid-play.** No engine studied does it for physics or rendering.
  CryEngine's audio swap is the only example found, and it is not worth the risk.

## 11. Order of work

Each step leaves a working engine.

| Order | What | Depends |
|---|---|---|
| **WO-076** | the registry, explicit registration, refusal by name, `none` per kind, `--list-providers` | — |
| **WO-085** | provider manifests, platform tables, `engine_provider()` in CMake | WO-076 |
| **WO-086** | the resolver, per-target selection in `project.json`, `engine_build --platform`, generated registration, `providers.lock.json` | WO-085 |
| **WO-087** | providers kept apart: no provider-to-provider links, `PROV-01`, an engine-owned debug-draw service, one create context | — |
| **WO-088** | the shipped player tested in CI: static providers, conformance and smoke, the provider symbol check | WO-086 |
| **WO-089** | librhi's backend order from `project.json`, and device capabilities refused by name | WO-086 |
| WO-077 | swap at dev time (shared libraries), now through the manifest and resolver | WO-076, WO-085 |
