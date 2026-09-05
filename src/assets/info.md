---
status: as-built
tier: hardened
verified: 2026-09-05
parses-external-input: true
covers:
  - src/assets/
tests:
  - tests/cooker_test.cpp
  - tests/cook_infra_test.cpp
  - tests/import_test.cpp
  - tests/decimate_test.cpp           # a level must be genuinely cheaper
  - tests/residency_test.cpp
  - tests/fuzz_mesh_loader_test.cpp   # cooked-binary parse the loaders depend on
  - tests/stress_assets.cpp           # garbage-in importer fuzz
  - tests/cook_hardening_test.cpp     # worker IPC framing + DDC GC
  - tests/cook_deps_test.cpp          # declared inputs must move the cook key
---
# Assets

## Purpose
The asset pipeline, end to end: stable identity, source-format import,
cooking to engine-native binaries, and loading those binaries back. Folder
structure mirrors the stages — and the library layering:

```
assets/
├── asset_ref.h            identity (engine_core-safe headers)
├── importers/             source formats → engine data   [engine_runtime*]
├── cookers/               source → .cache binaries        [engine_core]
└── loaders/               cooked binaries → GPU           [engine_runtime]
```
(*`stb_impl.cpp`/`cgltf_impl.cpp` are CPU-only implementation TUs and belong
to engine_core; the importer .cpps create GPU resources and are runtime.)

## Identity — AssetRef (`asset_ref.h`)
THE way scenes reference assets on disk:
`{"asset": "<uuid>", "path": "assets/models/mask.glb"}`.
UUID (assetlib) is the identity — survives sessions, machines, renames and
moves (the registry re-matches moved files by content hash at scan time).
The path is project-relative, never absolute. Resolution: uuid → registry →
current path; then relative path; legacy absolute paths tolerated read-only.
Session handles (registry slot indices) must NEVER be serialized.
`tools/scene_resave.cpp` migrates legacy scenes.

## Importers (`importers/`)
`IMeshImporter` implementations behind `ImporterRegistry` (extension →
importer): `GltfImporter` (cgltf) for glTF/GLB; `AssimpImporter` for FBX,
OBJ, COLLADA, 3DS, PLY, STL, Blend. FBX uses `PRESERVE_PIVOTS=false`
(see `src/animation/info.md` for the precision consequences).

Both importers MERGE every submesh/primitive of a source file into ONE `Mesh`
— a shared VB/IB (base-vertex-offset indices) with a `SubmeshRange` per source
part carrying its own material — the representation the cooker + renderer
already use. (Historically glTF read only `meshes[0].primitives[0]` and Assimp
returned only the first submesh, silently dropping the rest — see the resolved
`importers/issues.md`.) A single-submesh model stays on the simple single-draw
path (mesh.material set, `submeshes` empty). The whole model is skinned iff it
has a skeleton (one merged vertex format); a bone-less submesh inside a skinned
model binds rigidly to bone 0 so it can't collapse. Verified headless by
`tools/import_test.cpp` (bgfx Noop backend).

## Cookers (`cookers/`)
`assetlib::ICooker` implementations + `CookService`. Cooker headers include
`<assetlib/cooker.h>` (the contract alone) — not `cook_pipeline.h`; only the
orchestrator needs the pipeline. See `modules/assetlib/info.md` for how the
cook layer is split (orchestration / keying / dispatch / store / record
format / scheduling).
- `MeshCooker` — imports via Assimp (or cgltf for `.gltf`/`.glb`), writes
  vertex/index buffers + submeshes + bounds. Skinned meshes cook too: a
  re-import extracts the skeleton and embedded clips into the same binary.
  Also emits an **LOD chain** — see below.
- `TextureCooker` — stb decode → block-compressed texels + mips via
  `texture_encode`, in one of THREE families chosen by `COOK_TEX_TARGET`:
  `bc` (default; desktop + Steam Deck, via vendored rgbcx/bc7enc — ~200ms per
  4K BC1, BC7 final bake seconds not minutes), `astc` (iOS + modern Android,
  via the astc-encoder bimg already vendors), `etc2` (the GLES 3.0 floor).
  Encoder TUs pinned to -O2 even in Debug. BC is NOT available on any phone and
  ASTC is not on desktop AMD/NVIDIA, so this is a real fork in the output, not a
  quality tier. Notes:
    - ASTC and ETC2-RGB come from `bimg_encode`, which needed no new dependency
      (bgfx already vendors both encoders) but did need a patch —
      `bgfx.cmake__bimg-encode-missing-astcenc.patch`: upstream's source glob
      lists every 3rdparty encoder EXCEPT astcenc, so ASTC linked nowhere.
    - EAC is written here (`texture_eac.cpp`), because bimg has no EAC encoder at
      all — which would have left the ETC2 target unable to cook a cutout leaf
      (ETC2A alpha) or a normal map (EAC RG11), i.e. half of a real game.
    - Nothing goes to bimg at a partial-block size: its ETC2 loop indexes off
      the source with no edge clamp, and our mip chains end at 1x1. Mips are
      edge-replicated up to a whole block count first.
    - The runtime side refuses rather than guesses: `cooked_texture.h` used to
      default an unknown format id to RGBA8, handing block bytes to the driver
      as raw pixels. It now rejects unknown ids and formats the GPU cannot
      sample, naming the format and pointing at `COOK_TEX_TARGET`.
    - **That refusal moved to `gpu::textureFormatSupported()` (2026-09-05), and
      it must be asked BEFORE staging.** Refusing inside `createTexture2D`
      strands the staged payload for the life of the process — the backend frees
      staging memory only when a command consumes it — and on content cooked for
      the wrong target *every* texture takes that path, so the leak is the whole
      texture set. The predicate is thread-safe (caps are fixed at device
      creation), so a loader worker asks it before spending the memcpy. Pinned by
      `tests/gpu_seam_test.cpp`; the refusal is reported once per format, not
      once per texture.
- `SceneCooker` — scene JSON → binary for SceneService. Every read goes through
  `core/json_read.h`, not `nlohmann`'s own accessors: the const
  `operator[](size_type)` is UNCHECKED (`"position": []` indexes an empty vector)
  and `value(key, default)` THROWS on a key present with the wrong type, while the
  only try/catch here wraps the parse — so `"id": "3"` escaped the cooker onto
  CookService's background thread. A hand-editable file read by an unattended
  cooker gets the bounds- and type-safe accessors, always.
- `CookService` — drives the pipeline: background thread in the editor
  (`start()`, WAL SQLite allows concurrent main-thread reads), synchronous
  `cookOnce()` for the engine_cook CLI. Assets AND scenes cook as ONE task
  graph (`CookPipeline::cookGraph` + `assetlib::TaskGraph`): each stale
  scene is an ExtraTask with dependency edges on exactly the cooking assets
  it references (`collectSceneRefs`), so it cooks the moment its own assets
  land — the old flow cooked every scene sequentially after ALL assets.
  Scene tasks run on the worker pool with their own WAL read connections.

#### LOD chains (`cookers/mesh/decimate.{h,cpp}`)
A static mesh above `kMinTrianglesForLod` gets three coarser levels, cooked at
40% / 15% / 5% of its triangle count. A level ships only if it is at least 20%
cheaper than its parent (`kMinReductionRatio`) — a level that is not meaningfully
cheaper costs memory and a swap for nothing, which is precisely the state the R20
audit measured when nothing could decimate at all.

The algorithm is **vertex clustering**, chosen over quadric error metrics
deliberately: it cannot produce non-manifold output (there is no topology to get
wrong, and this runs unattended over content nobody inspects), it is O(n), and it
is deterministic — two machines must cook byte-identical levels or the shared DDC
stops being a cache. The cell representative is the cell's first vertex in index
order, never a centroid: a centroid drifts off the surface on thin geometry, and
averaging normals/UVs across a cell produces values belonging to no real vertex.

Authors specify a triangle *ratio*, not a grid resolution: grid resolution is
absolute, so one value gives 96% reduction on a dense mesh and 0% on a low-poly
prop. `decimateToRatio` bisects the resolution and returns the closest result it
found rather than failing — a level slightly off target is fine, a cook that
fails because a search did not converge is not.

Skinned meshes are skipped: clustering merges vertices across the mesh and would
have to reconcile bone indices and weights, which is a different algorithm.

#### The importers do not read glTF's roughness/metallic factors — on purpose
Both importers build materials through `Material::standard()`, and both pass the
engine defaults (0.7 / 0.0) rather than the source file's PBR factors.

For glTF that looks like a bug and is not. `roughnessFactor` and
`metallicFactor` default to **1.0** when absent, which most exporters omit, so
cgltf hands back 1.0/1.0 — and the forward pipeline samples only baseColor and
normal, never a metallicRoughness/ARM texture. glTF's factors are defined to
MULTIPLY that texture, so using them unmultiplied is not "the authored value",
it is a stand-in for a map nobody reads.

The MESH COOKER does read them, deliberately: a cooked asset records what the
source authored. So `fps_shooter`'s pistol is 1.0/1.0 in its `.cooked` and shades
fully metallic and fully rough — pinned by
`tests/cooked_format/tests/gltf_pbr_factors.rs` so the value is deliberate rather
than surprising (BUG-0012). The importers keeping the defaults while the cooker
records the source is a known inconsistency, and both are waiting on the same
thing: a renderer that samples MR/ARM.

Honouring them correctly means sampling the MR/ARM texture first. That is a
rendering feature, not an importer fix, and the defaults stay until it exists.

#### Material textures reach a shipped game
A `.material` names its textures by SOURCE path — the way an author types it —
and that path resolves through the registry. A **dist has no registry**, so the
same path resolves to nothing there: every textured material bound its white
fallback while every log line reported success. Exactly the failure meshes had
before their sibling `.ctex` files shipped.

`MaterialTexture::cooked` (`.cmat` v3) carries a CACHE-RELATIVE cooked path the
runtime can use with no registry at all. It is filled by **engine_build**, not
by the cooker, and that split is forced: a cooker can run in a worker PROCESS
that receives only source/output/uuid on argv, so it has no registry to resolve
against. The packager runs on the dev machine with the cache in front of it.

The rewritten `.cmat` is written into the PACKAGE, never back into the cache.
Cooked outputs are materialized from the DDC as read-only hardlinks, so the file
in `.cache` is the same inode as the content-addressed blob — rewriting it in
place fails, and would corrupt an entry shared with other projects and machines
if it did not. Pinned by `tests/package_closure_test.cpp`.

### Out-of-process cook workers
Every cook runs in a spawned `engine_cook_worker` child (one asset per
process, `src/tools/engine_cook_worker.cpp`): a corrupt FBX that SIGSEGVs
Assimp kills one worker and fails one asset — the try/catch-can't-trap-signals
problem is closed for real. Each child also runs under a HARD memory cap
(`setrlimit`, 2× the cooker's estimate, floor 1 GB, `COOK_TASK_MEM_CAP_MB`
pins it) and a kill deadline (`COOK_TASK_TIMEOUT_SEC`, default 3600s). The cap
is applied by the PARENT between `fork` and `execv` — rlimits are inherited
across `exec`, so it predates the child's first instruction including its static
initializers. (`posix_spawn` cannot express this: POSIX has no rlimit spawn
attribute. Windows already had the property, assigning its job object while the
process is still suspended.)
Outcome comes back via a sidecar result file (`RESULT/ERROR/OUTPUT/DEP`
lines), never stdout — cookers print freely. That file is FRAMED with a magic
header and an `END <lines> <digest>` trailer
(`assetlib/cook_result_file.h`), and the frame is validated before any field is
read: `RESULT ok` is the first body line, so a worker killed mid-write used to
leave a file that parsed as a clean success with its `OUTPUT` lines missing —
committing a mesh without its sibling textures, i.e. the silently-untextured
build arriving through the IPC channel instead of the packager. The worker binary must sit next
to the spawning executable; if missing (or `COOK_INPROC=1`), cooking falls
back in-process behind the exception net, with a loud warning. Crash/timeout
failures record the DDC key like any failure: not retried until inputs change
(or forceRecook). Fault-injection hooks `COOK_WORKER_TEST_CRASH` /
`COOK_WORKER_TEST_HANG` (substring of source filename) keep the containment
paths testable — a crash path you can't trigger is a crash path you never
verified. The parent's MemGovernor + worker-thread pool still schedule
admission; the child limits are the enforcement.

### Garbage collection: two stores, two policies
`engine_cook --gc` (dry run) / `--gc-prune` (delete) collects BOTH, in this order
and for a reason:

1. **The project `.cache/`, by REFERENCE** — reconcile files against the
   registry, allowlisting only extensions this GC understands, and fail closed if
   the registry is missing or empty (an absent DB means "I don't know what is
   referenced", not "nothing is"). Deleting here drops HARDLINKS, so it reclaims
   a coherent working directory rather than disk.
2. **The DDC, by BUDGET + LRU** (`DdcStore::collectGarbage`) — content-addressed
   blobs have no referrer to ask, and keys derive from inputs, so every edit or
   cooker bump orphans a blob permanently. This is where real disk comes back.

The order matters: step 1's deletions drop hardlinks, un-pinning blobs that step
2 would otherwise have to report as unreclaimable. Running the DDC first frees
nothing on the first invocation.

Rules: cookers may use Assimp/stb/assetlib but must never reference bgfx,
GLFW, or plugin symbols — `engine_cook` links engine_core alone. Failed cooks
must delete stale output; cooked formats carry versioned headers.

### Derived Data Cache (content-addressed cooking)
Full design + rationale: **`docs/architecture/asset-cook-architecture.md`**.

Cooking is a caching problem: cooked output is a pure function of
(source bytes ⊕ cooker id ⊕ cooker version ⊕ settings), so that hash — the
DDC key (BLAKE3-256) — names the output in a two-tier blob store
(`assetlib/ddc.h`). Local tier `~/.engine/ddc` (override `ENGINE_DDC`) is
per-machine, shared across projects; optional shared tier `ENGINE_DDC_SHARED`
(any network mount) is the studio cache — a hit there means a teammate/CI
already cooked it and nobody compresses that 8K texture twice. Staleness is
simply `record.ddcKey != currentKey`; wiping `.cache/` re-materializes by
hardlink without recooking; a per-cooker version bump re-cooks ONLY that
cooker's assets. A cook RECORD is a manifest of member blobs (cooked mesh +
its sibling `.ctex` embedded textures — cookers report extras via
`CookContext::addOutput`), fetched all-or-nothing. Cookers write to a TEMP
path, the pipeline ingests then hardlink-materializes — never hand a cooker a
hardlinked final path (an ofstream would truncate the blob for every project;
blobs are also stored chmod 0444 for exactly that reason). Cook identity per
cooker: `id()` + `version()` + `settingsFingerprint(ctx)` — the fingerprint
MUST cover every env knob that alters output (`COOK_TEX_HQ`, `COOK_TEX_TARGET`,
the normal-map filename heuristic), or a fast-quality blob silently satisfies a
final-bake request. `COOK_TEX_TARGET` is the sharpest case: a source PNG is
byte-identical whichever device the build is for, so without it in the key a
desktop machine's cached BC7 blob answers a phone's ASTC request and the SHARED
CACHE is what shipped undecodable blocks. Same shape as the arm64/x86 NaN
divergence in the decimator, and equally invisible on the machine that made it. Failed cooks store no blob but record the key: identical inputs are
not retried until source/cooker/settings change (`forceRecook` evicts the
local blob and bypasses the fetch path — otherwise it would just re-download
the bytes under suspicion).

## Loaders (`loaders/`)
`mesh_loader` — reads a `.cooked` mesh and creates GPU buffers **through
`render/gpu.h`, never through a graphics API directly** (G1a): it parses and
validates with no device present, and only the upload needs one. The fast
path that skips importers entirely. Validate the header version; mismatch
means "treat as missing" and fall back to import.

## Data Flow
```
source asset ── scan → registry.db (UUID, hash, state)
      │                      │ CookService (stale check)
      ▼                      ▼
  importers/  ←fallback─  cookers/ → .cache/*.cooked → loaders/ → GPU
```

## Future Work
- Quadric-error decimation (`meshoptimizer`) if a measurement ever says
  silhouette error is what hurts. Clustering trades quality for the guarantees
  above; it is the right default, not the endpoint.
- Custom asset type registration (cooker + loader pairs from plugins).

## ENGINE_COOK_DETERMINISM_CHECK — cook twice, compare, fail

The DDC names a cooked output by a hash of its **inputs**, so a cooker that is not
a pure function of those inputs makes the cache serve bytes that recooking would
not reproduce — and the disagreement travels to everyone who pulls from the shared
store. We shipped exactly that and found it by READING code: the decimator's
`(int32_t)std::floor(NaN)` is `INT_MIN` on x86-64 and `0` on arm64, so two
machines cooked different LOD levels from one mesh.

`ENGINE_COOK_DETERMINISM_CHECK=1` cooks every asset twice and compares. It lives
in `dispatchCook`, the one choke point both the serial and batch pipelines share,
so neither can bypass it. It compares:

- the **primary output's** bytes (`blake3File` — the same hash the DDC keys on);
- every **extra output** by BASENAME, because the second cook writes into a
  directory of its own so absolute paths cannot match. A cooker deterministic in
  its primary and not in its sibling `.ctex` blobs is still broken;
- the **recorded dependencies**, which feed the next cook's key, so a cooker that
  varies here makes staleness itself non-deterministic;
- and whether the second cook **failed at all** — one-shot state looks fine in a
  clean build and breaks the moment anything cooks twice.

A mismatch **fails the cook.** A warning would be read as advisory, and the damage
is not local.

An ENVIRONMENT VARIABLE rather than a build flag, copied from vCAD's
`CAD_PLUGIN_DETERMINISM_CHECK`: it can be turned on against a build that already
exists, on the day an artist says an asset looks different on the build machine.
Off by default because it doubles every cook.

`tests/cook_determinism_test.cpp` proves the check CATCHES each drift mode with a
fake cooker per failure, because a determinism check that only ever passes cannot
be told apart from one that does nothing. It runs twice under ctest — once with
the flag on, once off — since a check that fires when disabled would double every
cook and the flag would be turned off and stay off.

**Standard headers are declared explicitly here, not inherited.** libc++ (macOS)
pulls much of the standard library in transitively and libstdc++ (Linux) does
not, so a file using `std::memcpy` with no `<cstring>` or `std::string` with no
`<string>` builds on the development machine and fails on every Linux leg. That
cost three CI round-trips one error at a time, because a build stops at the first
failure. `scripts/check_std_includes.py` now finds them all in one local pass and
runs as the `std_includes` unit test — it caught two of its author's own the
moment they were written.
