---
status: as-built
verified: 2026-08-03
covers:
  - modules/assetlib/src/cook/*.cpp
  - modules/assetlib/src/ddc/*.cpp
  - modules/assetlib/src/task_graph.cpp
  - src/assets/cookers/
  - src/tools/engine_cook.cpp
  - src/tools/engine_cook_worker.cpp
tests:
  - tests/cook_infra_test.cpp
  - tests/cooker_test.cpp
  - tests/fuzz_ddc_manifest_test.cpp
---
# Offline Asset Cook Architecture

> **Status:** Sections 1–5 document the architecture **as built** — content-
> addressed DDC, process-isolated workers, and the cost-weighted task graph are
> all in the build and exercised by `ctest -L unit`. Section 6 (*cookers as
> transformation graphs*) is the **target** architecture, not yet started; it is
> written down here because the rule that governs it (§6.2, stage-boundary
> economics) is the part most likely to be got wrong by a well-meaning future
> change. Section 8 records what we deliberately did **not** build, and why.

## 0. The whole thing in one picture

Source files get a UUID, get **cooked** into GPU-ready binaries, the cooked bytes
get cached by their **content hash** so nothing is ever cooked twice, and at
runtime the engine hands those binaries to the GPU with no parsing.

```
   AUTHORING                 OFFLINE (cook time)                RUNTIME
   ─────────                 ───────────────────                ───────

  rust_albedo.png ─┐
  pistol.glb       │      ┌──────────────┐
  standard.shader  ├─────►│ AssetRegistry│  UUID + BLAKE3 of the bytes
  rust.material    │      │  (SQLite)    │  — a ledger, not a cache
  main.scene      ─┘      └──────┬───────┘
                                 │  "which of these are stale?"
                                 ▼
                          ┌──────────────┐
                          │ CookPipeline │  task graph, memory + thermal budget
                          └──────┬───────┘
                    ┌────────────┼────────────┐
                    ▼            ▼            ▼
              TextureCooker  MeshCooker  ShaderCooker    each in its own
                    │            │       MaterialCooker  child process
                    └────────────┼────────────┘          SceneCooker
                                 ▼
                          ┌──────────────┐
                          │     DDC      │  content-addressed blob store
                          └──────┬───────┘  ~/.engine/ddc (+ shared tier)
                                 │ hardlink
                                 ▼
                        <project>/.cache/<type>/<uuid>.cooked
                                 │                    ▲
                                 │              collectGarbage() prunes
                                 ▼              what nothing claims
                          ┌──────────────┐
                          │ AssetService │  residency budget, LRU
                          └──────┬───────┘
                                 ▼
                       ┌────────────────────┐
                       │  GpuResourceCache  │  content key, refcount, budget
                       └─────────┬──────────┘
                                 ▼
                RenderWorld → visibility → ForwardPipeline → screen
```

### One file, end to end

**1 — The registry notices.** `AssetRegistry::scan()` assigns a UUID and records
a BLAKE3 hash of the file's bytes in `<project>/.cache/registry.db`. It is a
*ledger*: it says what exists and what state it is in, and stores no content.

**2 — Is it stale?** The recipe is hashed, not just the file:

```
DDC key = BLAKE3( cooker id ⊕ cooker version ⊕ settingsFingerprint ⊕ source hash )
```

The **cooker id** namespaces the key, which is why bumping `TextureCooker::kVersion`
re-cooks textures *and only textures*. **`settingsFingerprint`** carries
everything else that changes output for identical source bytes — the texture
quality tier; for shaders, the target profiles, the `shaderc` binary's mtime, and
a transitive hash of the `.sc`/`.sh` sources (those are inputs but not registry
assets, so nothing else can see them).

**3 — The DDC answers.** On a **hit** it *hardlinks* the existing blob into
`.cache` — no work. This is the 8 min → 1.8 s difference, and with
`ENGINE_DDC_SHARED` a teammate never re-cooks what you already cooked. (Hardlinks
are also why `du` under-reports `.cache`: the entry and the DDC blob are one
inode.) On a **miss**, the cooker actually runs.

**4 — Cooking.** Each cook runs in an `engine_cook_worker` child process, because
a corrupt FBX can `SIGSEGV` and no `try/catch` traps that; in a child it kills
one asset instead of the editor. Two governors decide *when*: a **memory** budget
admits work against `estimatePeakBytes()` (an 8K texture decodes to 256 MB, so
those serialize while cheap cooks pack in parallel), and **thermal citizenship**
caps workers at cores−2 at background QoS.

**5 — Output lands** at `<project>/.cache/<type>/<uuid>.cooked`, and the DDC
records a manifest of every file that cook produced — including siblings, so a
cache hit on another machine materializes the whole set.

**6 — GC.** `collectGarbage()` walks `.cache` and deletes what the registry no
longer claims (deleted sources, outputs orphaned by a version bump). It is
deliberately paranoid — a missing *or empty* registry aborts the sweep, because
`AssetRegistry::open()` **creates** a missing database, so a naive "does the DB
exist?" check passes against a brand-new empty one and everything looks orphaned.

**7 — Runtime.** `AssetService` reads cooked bytes under a residency budget with
LRU eviction. Underneath, `GpuResourceCache` is content-keyed: the same texture
requested twice returns the *same* GPU handle with a bumped refcount.
`refs == 0` means **evictable, not dead** — eviction is a budget decision made
later, never mid-frame, because dropping a resource a queued draw points at is a
use-after-free.

### Shaders and materials

`standard.shader` is a manifest naming its `.sc` sources **and declaring its
interface** — which parameters a material may set, their types, defaults, and
where each lands in GPU memory. `ShaderCooker` compiles the (features × profiles)
matrix, then **reads the compiled bytecode's own uniform table back and verifies
the declaration against it, on every variant**. Without that check the manifest
could lie and a material would set a value that writes nowhere.

`rust.material` names a shader and supplies values. `MaterialCooker` resolves
those names against the declared interface *at cook time* and emits finished
float blocks, so the runtime does no lookup and no validation — it uploads bytes.
Unknown name ⇒ failed cook. Unset parameter ⇒ the shader's default, never a gap
(a sparse block would inherit whatever the previous draw wrote).

See `src/assets/cookers/shader/info.md` and `src/assets/cookers/material/info.md`.

> **State:** `.cshader` is live — a shipped `fps_shooter` dist renders its
> standard forward program from cooked bytes, resolved by the name inside the
> file (a dist has no registry). `.cmat` is cooked and tested but still inert:
> `ForwardPipeline` reads the fixed `Material` struct. See
> `docs/process/roadmap.md`.

## 1. The reframe: cooking is a caching problem

The naive cooker walks every asset and compresses it here, now, on all cores.
That model makes every machine redo the entire studio's compression work, and
it melted a laptop (`engine_cook fps_shooter`, 8 minutes, all 12 cores pinned).

Every production cooker is built on one observation instead:

> The cooked output of an asset is a **pure function** of
> (source bytes ⊕ cooker identity ⊕ cooker version ⊕ settings).

Hash those inputs and the hash *names* the output. Cooking then means: compute
the key, and if anyone — you, a teammate, CI — has already produced that
output, fetch the finished blob instead of recomputing it. At studio scale most
of a cook is cache hits: the 8K texture is BC-encoded once, ever, by anyone.

Everything below follows from that one idea.

## 2. Layers

Cook code is split one concern per translation unit. `CookPipeline` orchestrates
and owns no mechanism.

| unit | concern |
|---|---|
| `assetlib/cooker.h` | The **cooker contract**: `CookContext` / `CookResult` / `ICooker`. No pipeline dependency — cooker implementations and `engine_cook_worker` include only this. |
| `src/cook/key.{h,cpp}` | **Identity + staleness.** Builds the DDC key; `cookIsStale()` is the entire "is the cooked output already correct?" policy. |
| `src/cook/dispatch.{h,cpp}`, `src/cook/worker_posix.cpp`, `src/cook/worker_win32.cpp`, `src/cook/result_file.cpp` | **Execution mode.** Isolated child process vs in-process behind an exception net. `dispatchCook()` is the seam every cook passes through — and the hook for remote/farm execution. |
| `assetlib/ddc.h`, `src/ddc.cpp` | **The store.** Two-tier content-addressed blobs. |
| `assetlib/ddc_manifest.h`, `src/ddc_manifest.cpp` | **Cached-output record format.** A cook's output set as a manifest of per-member content-hashed blobs. |
| `assetlib/task_graph.{h,cpp}` | **Scheduling.** Cost-weighted DAG, memory-budget admission, thermal governance. |
| `src/cook_pipeline.cpp` | **Orchestration** only: what to cook, in what order, what the registry records after. |
| `src/cook_env.h` | The `COOK_*` env-knob reader. |
| `src/assets/cookers/` (engine) | The concrete cookers (`MeshCooker`, `TextureCooker`, `SceneCooker`) and `CookService`, the editor/CLI driver. |

## 3. Identity and the DDC

### 3.1 Key recipe

```
cook key = blake3( "engine-ddc-v1"          // recipe version — see §5.2
                 ⊕ cooker->id()             // "mesh", "texture"
                 ⊕ cooker->version()        // per-cooker, NOT global
                 ⊕ cooker->settingsFingerprint(ctx)
                 ⊕ record.importSettings
                 ⊕ blake3(source bytes)
                 ⊕ Σ declaredInputs         // extra FILES the cook reads
                 ⊕ Σ dependency source hashes )
```

Fields are length-prefixed so no two different input sets can concatenate to
the same byte stream. The two Σ sets are sorted, so neither the order a cooker
declares its inputs in nor the row order of the dependency table can change the
key.

**The key is the ONLY invalidation mechanism.** §3.2's staleness test reads the
key and nothing else — it never consults `record.state` (bar keeping a Failed
record failed). So a registry-side "mark dependents stale" cascade would be a
no-op, and `transitiveDependents()` is a query for tooling, not an invalidation
path. A key is also the only thing that is correct across a SHARED store: a
cascade is local to one machine's registry, while another machine fetches by
content key and would be served the stale blob regardless.

**Per-cooker versions are the point.** A single global `kCurrentCookVersion`
(what this replaced) meant a texture-encoder change re-cooked every mesh in the
project. Now bumping `TextureCooker::kVersion` re-keys textures and nothing
else. Current versions: `MeshCooker` 12, `TextureCooker` 3.

**`settingsFingerprint` must cover every input that alters output but isn't the
source bytes.** Today that is `COOK_TEX_HQ` (BC7 final-bake vs fast BC1/BC3)
and the filename normal-map heuristic (BC5 + linear mips). Miss one and the
cache serves the wrong quality tier — silently, and across the whole studio.

**Extra FILES go through `declaredInputs()`, not `settingsFingerprint`.**
`CookContext::addDependency` takes a UUID and cookers have no registry lookup, so
a cooker whose second input is a plain file (a shader's `.sc` stage sources, the
`.shader` manifest a material resolves against) once had no way to declare it and
hand-rolled `blake3File()` into its fingerprint instead. That worked and could not
be checked: a cooker that forgot looked identical to one with no extra inputs.
Declaring the paths lets the pipeline do the hashing, and makes the omission a
test failure — `cook_deps_test` perturbs every declared input of every registered
cooker and requires the key to move.

Deliberately **source** hashes, never the dependency's cooked key: folding cooked
identity in would make the fold transitive and pull in inputs the dependent
provably does not read. A material depends on a shader's declared INTERFACE, not
on its shading code, so keying materials on the `.sc` bytes would recook every
material in the project on every shader edit. See §8 for why the Merkle-DAG
variant of this was rejected.

### 3.2 Staleness

`cookIsStale()` is the whole policy, and it is deliberately not mtime-based:

- stored key ≠ current key → **stale** (inputs changed, or never attempted)
- same key, `Failed` → **not stale**. These exact inputs already failed; retry
  only when something changes, or via `forceRecook()`.
- same key, empty `cookedPath` → **not stale** (deliberately skipped, e.g. a
  skinned mesh the runtime import path handles)
- same key, materialized output missing → **stale**. Someone wiped `.cache/`;
  a DDC hit restores it without recooking.

### 3.3 Two tiers

- **Local**: `~/.engine/ddc` (`ENGINE_DDC`) — per *machine*, shared across every
  project on the box.
- **Shared**: `ENGINE_DDC_SHARED`, any path both machines can see. An NFS/SMB
  mount is a studio cache with zero server code.

Read path is local → shared, and a shared hit *promotes* the blob into local so
the next fetch never touches the network. Write path ingests local, then pushes
shared **best-effort** — a dead mount must never fail a cook that already
produced correct output.

Blobs live at `<root>/<first 2 hex>/<key>.blob`, fanned out so no directory
collects millions of entries.

### 3.4 Records are manifests (action cache over CAS)

A cook can produce several files: a cooked mesh *plus* sibling `.ctex` blobs for
its embedded textures. Each member is stored under **its own** content hash, and
a small manifest under the cook key names them:

```
ddc-manifest-v1
<blobKey>\t@                 <- the primary output
<blobKey>\tmesh_t0.ctex      <- a sibling, plain filename only
```

Fetch materializes every member or reports a miss — a hit can never yield a mesh
whose textures don't exist. Two useful properties fall out: identical texture
bytes across different assets dedup to one blob automatically, and this is
exactly an **action-cache entry pointing into a CAS**, which is what §6 builds on.

Member names arriving from a *shared* store are remote input written by another
machine, so they are validated by allowlist — a bare filename, nothing else. A
blocklist of `/ \ ..` still lets `C:evil` escape on Windows.

## 4. Execution and scheduling

### 4.1 Process isolation

Every cook runs in an `engine_cook_worker` child, one asset per process. A
corrupt FBX that SIGSEGVs Assimp kills the child, not the editor — which is the
only real fix, since `try/catch` cannot trap signals. Outcome returns via a
sidecar result file (`RESULT` / `ERROR` / `OUTPUT` / `DEP` lines), never stdout,
because cookers print freely. Unknown keys are ignored, so the protocol can gain
`WARNING` / `LOG` / `STAT` later without a break.

Each child also gets a **hard** memory cap — 2× the cooker's estimate, 1 GB
floor: `setrlimit(RLIMIT_DATA|RLIMIT_AS)` on POSIX, and on Windows a job object
with `JOB_OBJECT_LIMIT_PROCESS_MEMORY` assigned while the process is still
suspended, so the limit predates the child's first instruction. Plus a SIGKILL
deadline (`COOK_TASK_TIMEOUT_SEC`, default 1 h — an HQ 8K BC7 bake is
legitimately minutes).

Missing worker binary or `COOK_INPROC=1` falls back to in-process behind the
exception net, **loudly** — silently losing crash isolation is how a "stable"
editor starts dying on corrupt imports again.

`COOK_WORKER_TEST_CRASH` / `COOK_WORKER_TEST_HANG` (matched against the source
filename) trigger the containment paths on demand. A crash path you cannot
reproduce is a crash path you have never actually tested, so these must exist on
every platform we port to.

### 4.2 The task graph

Cook work is a DAG of cost-weighted tasks, not a flat parallel-for.

- **Longest-first dispatch.** The ready queue is a max-heap on estimated bytes
  (LPT scheduling), so an 8K texture starts at *t=0* instead of straggling
  behind a hundred trinkets.
- **Two lanes.** `work()` runs on the worker pool; `done()` runs *serialized on
  the caller thread*. That is what lets registry commits keep their single-
  connection discipline while cooks overlap.
- **Dependents release on DRAIN, not success.** A failed asset never wedges the
  tasks behind it.
- **Cycles are reported, not deadlocked** — and named (`B -> C -> A -> B`),
  because "these 40 tasks are unreachable" tells you nothing about which edge
  to delete.

Scenes join the same graph as `ExtraTask`s with edges on exactly the cooking
assets they reference, so a scene cooks the moment *its own* assets land instead
of every scene waiting for the whole batch.

### 4.3 Thermal citizenship

An offline cook is a background chore, not the foreground app. Three levers,
all owned by `TaskGraph`:

- worker cap of **cores − 2** (`COOK_THREADS`)
- QoS demotion (`QOS_CLASS_UTILITY` / `nice(10)` / `BELOW_NORMAL_PRIORITY_CLASS`)
- **memory-budget admission**, not a thread count (`COOK_MEM_BUDGET_MB`, default
  60% RAM). Tasks reserve `estimatePeakBytes()`; a burst of 8K textures
  serializes instead of OOM-ing. A task larger than the whole budget runs
  *alone* rather than deadlocking on space that will never exist.

The governor **schedules** by estimate; the child memory cap **enforces**. Both
are needed — an estimate is not a measurement.

## 5. Invariants

These are load-bearing. Breaking one is usually silent.

1. **Cookers write to a temp path, never the final path.** Materialization
   hardlinks CAS blobs; an `ofstream` on a hardlinked output would truncate the
   blob for every project sharing it. Blobs are also stored read-only (0444) for
   the same reason.
2. **The key recipe is versioned** (`"engine-ddc-v1"`). Any change to how keys
   are built invalidates every cache everywhere — treat it as a deliberate,
   announced event.
3. **Registry writes happen only on the drain lane.** One writer connection,
   many readers (WAL). Worker-side code (e.g. scene tasks) opens its own read
   connection.
4. **Cancelled ≠ failed.** A cancelled cook must record **nothing**. Writing
   `Failed` would stamp the record with the *current* key, and staleness reads
   "same key + Failed" as "already attempted" — the asset would never cook again
   until its source changed. `CookResult::cancelled` exists for exactly this.
5. **Determinism.** Same inputs must yield byte-identical outputs, or a shared
   cache serves subtly different content to different machines. Pin compressor
   versions; no time, thread order, or iteration-order nondeterminism in output.
6. **`CookContext` is single-threaded by contract.** `addDependency` /
   `addOutput` append to plain vectors owned by one scheduling task. A cooker
   that parallelizes its *own* work must funnel these back to its `cook()`
   thread.
7. **The regression test for any cook change is a warm pass that recooks
   nothing.** Since staleness is purely key comparison, zero recooks on an
   already-cooked workspace proves every key is byte-identical. This is how the
   monolith decomposition was verified; repeat it after any future cook
   refactor. Unit coverage: `tests/cook_infra_test.cpp`, `tests/cooker_test.cpp`.

## 6. Target: cookers as transformation graphs

Today one `ICooker` is one monolithic transformation, source file → final asset.
The target is a graph of independent, individually cacheable stages: parse,
validate, tangents, LOD, optimize, meshlets, compress, package. Each stage
becomes independently cacheable, parallelizable, and inspectable — turning the
cook pipeline into a genuinely incremental build system.

### 6.1 The model

A stage is a pure function from named input artifacts to named output artifacts.
Artifacts are **files**, which is what lets a stage be content-addressed *and*
shipped across a process or machine boundary.

```
action key = blake3( stage_id ⊕ stage_version ⊕ settings
                   ⊕ content hashes of input artifacts )
```

You cannot know an output's hash before running it, so lookup is two-level: an
**action cache** (action key → manifest of output artifact hashes) over a **CAS**
(hash → bytes). §3.4 is already precisely this shape — the storage layer needs
no redesign, only a generalization of the key recipe from one action per asset
to one action per stage.

The whole cook becomes a Merkle DAG: change a leaf, and only the stages
downstream of it re-run.

```
source.fbx ──[mesh.parse]──> scene.geom ──┬─[mesh.process]─> proc.geom ─[mesh.emit]─> .cooked
                              (cached)     ├─[mesh.skeleton]────────────────────────> .ozz
                                           ├─[mesh.clips]──────────────────────────> .anim
                                           └─[tex.encode ×N]───────────────────────> .ctex
```

### 6.2 THE RULE: stage-boundary economics

**A stage boundary costs a serialize + store of the intermediate. It only pays
when recompute cost exceeds that, and when the downstream is volatile while the
upstream is stable.**

This is the single most important thing in this document, because the naive
"decompose everything" reading of §6 makes cooks *slower*. Do not add a stage
boundary without doing this arithmetic.

Worked, with real numbers from this engine:

| pipeline | analysis | verdict |
|---|---|---|
| **Texture**: decode → mips → block compress | Whole pipeline is ~200 ms for a 4K (rgbcx). The mip-chain intermediate is ~85 MB. Writing 85 MB to the DDC to avoid 200 ms of encode is a straight loss. | **Do not split.** The monolith is correct. |
| **Mesh**: parse → process → emit | Assimp parsing a large FBX is *seconds*, and Assimp's version changes almost never. Everything downstream (tangents, LODs, meshlets, optimization) is exactly where new features land. Textbook stable-upstream / volatile-downstream. | **Split.** |

A corollary that removes most of the storage-amplification worry: **on a
final-stage cache hit, no intermediate is ever materialized.** Intermediates are
fetched only when a downstream stage actually has to re-run.

### 6.3 Two wins already visible

Not speculative — both are observable in current cook logs:

1. **Skinned meshes are parsed twice.** `MeshCooker::cook` notes "both Assimp
   scenes of this asset count as ONE resident import" — it re-imports the file
   for the skinned path. A cached parse artifact makes that one parse feeding
   the mesh, skeleton, and clip outputs.
2. **Embedded textures encode serially inside one task.** The `pistol` asset
   logs `4096 BC1 188ms`, `4096 BC5 104ms`, `4096 BC1 187ms`, `4096 BC5 98ms` —
   ~580 ms sequential inside a *single* graph node while other cores idle. As
   four stages they become four nodes and the existing cost-weighted scheduler
   parallelizes them for free.

Secondary benefit: `estimatePeakBytes` per *stage* is far more predictable than
per whole cooker (parse from file size, encode from dimensions), so memory
admission stops guessing.

### 6.4 Interface sketch

Deliberately mirrors `ICooker`'s four DDC-identity members so `src/cook/key.h`
generalizes rather than forks:

```cpp
class IStage {
public:
    virtual const char* id()      const = 0;
    virtual uint32_t    version() const = 0;
    virtual std::string settingsFingerprint(const StageContext&) const { return {}; }
    virtual size_t      estimatePeakBytes(const StageContext&) const;
    virtual StageResult run(StageContext&) = 0;   // input paths in, output paths out
};
```

### 6.5 Migration

Incremental, never a big-bang rewrite. Every phase leaves a working cooker.

- **Phase 0 — plumbing, zero behavior change.** Add `IStage`, artifacts, and
  action keys beside `ICooker`. `CookPipeline` gains the ability to expand one
  asset into a sub-DAG. A cooker that does not opt in stays exactly one stage —
  which is what it already is.
  **Hard constraint:** for a single-stage cooker the action key must be
  byte-identical to today's cook key, or the whole workspace re-cooks. Verify
  with invariant §5.7.
- **Phase 1 — split `MeshCooker`** into parse / process / emit plus per-texture
  stages. Kills the double parse, parallelizes the embedded encodes. Meshes
  re-cook once (expected — the version bumps anyway); textures don't move.
- **Phase 2 — new stages as features:** tangents, LOD generation, meshlets.
  Each independently cacheable, so iterating on LODs stops re-parsing FBX.
- **Phase 3 — reconsider textures** only if measurement justifies it. Current
  expectation: it won't. See §6.2.

### 6.6 Risks

- **Determinism becomes load-bearing at every boundary,** not just the final
  output. If a parse stage's output ordering is unstable, misses cascade through
  the entire downstream chain. Wants an explicit "same input twice → identical
  artifact bytes" test before we rely on it.
- **Graph size.** Six stages × 10k assets is 60k nodes. Scheduling copes;
  diagnostics need an `--explain <asset>` mode showing which stages hit or
  missed and why.
- **Complexity is a real cost.** The current monolith is *readable*. Splitting
  what doesn't pay makes the code worse for no gain — hence §6.2.

## 7. Environment knobs

| variable | effect |
|---|---|
| `ENGINE_DDC` | Local DDC root (default `~/.engine/ddc`). |
| `ENGINE_DDC_SHARED` | Shared DDC root. Unset = local only. |
| `COOK_THREADS` | Worker count (default cores − 2). |
| `COOK_MEM_BUDGET_MB` | Admission budget (default 60% RAM). |
| `COOK_TASK_MEM_CAP_MB` | Hard per-child cap (default 2× estimate, ≥ 1 GB). |
| `COOK_TASK_TIMEOUT_SEC` | Per-task kill deadline (default 3600). |
| `COOK_INPROC=1` | Force in-process cooking (no crash isolation). |
| `COOK_TEX_HQ=1` | BC7 final-bake quality. Part of the cache key. |
| `COOK_WORKER_TEST_CRASH` / `_HANG` | Fault injection, matched on source filename. |

## 8. Decisions deliberately not taken

Recorded so they aren't re-litigated as oversights. Each is defensible to
revisit *with a reason*; none is free.

- **Batching registry commits into one transaction.** Per-asset commit is a
  deliberate crash-resilience trade: partial cook progress survives a kill.
  Batching a minutes-long cook into one transaction throws that away.
- **A retry policy for transient failures.** Needs a transient-vs-deterministic
  classification to be worth anything; without it, it just retries malformed
  assets N times.
- **Dynamic memory re-reservation** (tasks growing their reservation mid-run).
  The child memory cap already prevents catastrophe; scheduling quality
  degrades gracefully.
- **Promoting `TaskGraph` into a Core module.** It is generic enough to serve
  shader compilation, navmesh bakes, light bakes, and packaging, and probably
  should move eventually — but it is a cross-module move with no functional
  gain today.
- **A packed-blob DDC backend** (packfiles, as Git/Bazel end up with). Correct
  eventually for millions of files; premature now.
- **Multi-resource scheduling** ("2 CPU cores, 1 GPU encoder, 500 MB"). The
  right generalization once tasks actually contend for something other than RAM.
- **LPT starvation mitigation** (aging / best-fit admission). A cook graph is a
  *finite* batch and every completion re-notifies, so small tasks cannot be
  postponed indefinitely. It is a latency nuance, not starvation.

## 9. Where this came from

Sequenced roadmap, all committed: thermal citizenship → scene-scoped on-demand
cooking → fast texture formats → content-addressed DDC → out-of-process workers
→ rgbcx/bc7enc encoders → task-graph scheduler → monolith decomposition → audit
fixes. Cumulative effect on `fps_shooter`'s cold cook: **8 min → 1.8 s**, with a
wiped `.cache` restoring from the DDC in ~107 ms and a warm no-op at ~37 ms.

Remaining roadmap beyond §6: remote execution / build farm (the
`dispatchCook()` seam plus the worker file protocol is where it plugs in), an
HTTP/S3 shared-DDC backend behind the same key space, and out-of-core cooking
for single assets larger than RAM.
