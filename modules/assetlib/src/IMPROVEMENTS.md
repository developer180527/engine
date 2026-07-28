# Issues and Imporvemenrts (Tue Jul 28)

---
## RESOLUTION (verified against source 2026-07-28)

Every claim re-checked against the code before acting. Regressions for the
fixed paths live in `tests/cook_infra_test.cpp` (scheduler, DDC, manifest
parser, registry scanner) — `ctest -L unit`.

### Assessment 1 — all four TRUE, all fixed
| claim | verdict | fix |
|---|---|---|
| `storeBytes` temp path uses pid only → concurrent same-key writers collide | **TRUE** | `ingest()` had a pid+atomic-counter temp name; `storeBytes` didn't. Both now share `uniqueTempPath()`. Two cooks of duplicate assets (or two meshes with byte-identical embedded textures) hit the same key. Test: 8 threads, same key, blob intact + no temp leaked. |
| `waitpid` treats EINTR as a dead child | **TRUE** | EINTR now retries; a genuine reap error reports separately. The old code both misreported healthy cooks as crashes AND leaked the child (left the loop without reaping). |
| manifest name check omits `:` (Windows drive-relative) | **TRUE** | Replaced the blocklist with an allowlist (`isPlainFilename`): rejects separators, `:`, `.`/`..`, control chars, and anything `fs::path` doesn't agree is a bare filename. Shared-DDC manifests are remote input. |
| `scan()` calls `all()` per new file → O(N²); manual BEGIN/COMMIT leaks the transaction | **TRUE (both)** | Move detection now builds a hash index **once** (lazily — an up-to-date tree never pays), plus a `claimed` set so one record can't be adopted by two files (the old per-file `all()` re-read got that for free via `seen`). Transaction is an RAII guard that ROLLBACKs unless committed; the directory walk uses the `error_code` overload. |

### Assessment 2
**Fixed:** 1 (documented the `CookContext` single-thread contract — synchronizing
it would add a mutex to a hot path for a hypothetical), 3 (MemGovernor
saturates + shouts instead of underflowing into a permanent scheduler wedge),
4 (cancellation now reaches the cooks: workers poll a predicate and SIGKILL
their child, so quitting no longer waits out a multi-minute bake — and
`CookResult::cancelled` makes `commitResult` record **nothing**, because a
cancelled cook stored as Failed would carry the current DDC key and staleness
would never retry it), 10 (`unordered_map<UUID,int>` — `std::hash<UUID>`
already existed), 13 (cycle reporting now walks predecessors and prints the
actual loop, e.g. `B -> C -> A -> B`).

**Partly false — 9 (drain-thread exception safety):** `TaskGraph::run` already
wraps *both* lanes in `catch(...)` (task_graph.cpp), and `CookService` wraps
each pass. The one real hole was `fs::relative` in `commitResult` using the
throwing overload — switched to `error_code`.

**Overstated — 7 (LPT starvation):** a cook graph is a *finite* batch and every
completion re-notifies, so small tasks cannot be postponed indefinitely; and a
task larger than the whole budget is admitted alone rather than blocking. It's
a latency/fairness nuance, not starvation. Not changed.

**Already true — 12:** the worker parser ignores unknown keys, so `WARNING`/
`LOG`/`STAT` can be added later without breaking the protocol. No producers
added until something consumes them.

**Deliberately not done (roadmap, not fixes)** — these are real but are
projects with their own design cost, and doing them speculatively would add
unused surface: 2 (dynamic memory re-reservation; the child `setrlimit` cap
already prevents catastrophe), 5 (batching registry commits — the current
per-asset commit is a deliberate crash-resilience tradeoff: partial cook
progress survives a kill), 6 (retry policy — needs a transient/deterministic
classification to be worth anything), 8 (promote TaskGraph to Core — a
cross-module move with no functional gain today), 11 (packed blob backend),
14 (multi-resource scheduling), 15 (cookers as transformation graphs — the
most valuable long-term item, and the biggest).

**16:** no action needed; agrees with the audit.
---

## Assessment 1: 
In ddc.cpp, storeBytes constructs temporary file paths using only the process ID (m_local / (key + ".bytes." + std::to_string(getpid()))), which causes thread collisions and file corruption when concurrent worker pool threads write to the same DDC key simultaneously. Over in cook_dispatch.cpp, the cookInWorkerProcess function reaps child worker processes but breaks out with status = -1 whenever waitpid returns -1, misinterpreting benign EINTR signal interruptions as fatal worker process crashes. Meanwhile, ddcFetchRecord in ddc_manifest.cpp validates member filenames against forward slashes, backslashes, and .. to block path traversal, but omits colon checks (such as C:), leaving Windows targets vulnerable to relative drive writes. Lastly, AssetRegistry::scan in asset_registry.cpp packs two major flaws: it calls all()—and thus re-executes SELECT * FROM assets—inside a recursive directory iteration loop to check for moved files, turning your scan into an $O(N^2)$ disk-I/O nightmare, and it manually manages transaction boundaries with BEGIN; and COMMIT; without an RAII guard, which leaves the SQLite database handle permanently locked if a filesystem exception occurs mid-scan. 

## Assessment 2:

1\. Make cooker callbacks explicitly thread-safe

The current implementation assumes that every cooker executes on a single thread. The callback lambdas capture and mutate w.deps and w.outputs directly using std::vector::push_back(). This is safe today because each CookContext appears to be used by only one worker thread, but it becomes undefined behavior if a future cooker parallelizes its own work (for example, parallel texture compression or mesh optimization). Either document that CookContext::addDependency() and addOutput() are strictly single-threaded APIs or make these callbacks internally synchronized.

2\. Memory governor depends entirely on estimation accuracy

The TaskGraph admits work according to estimatePeakBytes(), which is an estimate rather than a measurement. If a cooker significantly underestimates its memory usage, the scheduler may allow too many large tasks to execute simultaneously. The worker process memory cap prevents catastrophic failure, but scheduling quality degrades considerably. A more robust design would allow tasks to reserve additional memory dynamically or report memory growth during execution instead of relying on a fixed reservation taken before the task begins.

3\. Defensive checks in MemGovernor

release() performs

used -= need;

without validating that used >= need. Any future bug causing a double release or mismatched acquire/release pair would silently underflow the counter and permanently block future scheduling. An assertion (or saturating check in release builds) would make debugging much easier.

4\. Cancellation is only scheduler-level

When cancellation occurs, the scheduler stops dispatching new work but waits for all currently executing tasks to finish. That works well for short jobs, but a large texture compression or mesh optimization may continue running for tens of minutes. Consider introducing a lightweight cancellation token into CookContext so long-running cookers can periodically abort gracefully when requested.

5\. Registry updates serialize the pipeline

All registry modifications occur on the drain thread after worker completion. This is the correct choice for thread safety, but once projects reach hundreds of thousands of assets, registry commits may become a bottleneck. A future optimization would batch multiple completed assets into a single database transaction or registry update pass.

6\. Missing retry policy

Failures such as temporary filesystem issues, intermittent network storage outages, or transient shared DDC failures immediately become permanent cook failures. A configurable retry policy for transient errors would improve resilience without affecting deterministic failures such as malformed assets.

7\. Longest-processing-first may cause starvation

Scheduling tasks by estimated memory size is an excellent heuristic for maximizing throughput, but very large tasks can monopolize the available memory budget while many tiny jobs remain queued. Over time, introducing aging, weighted priorities, or a "best-fit" admission policy would improve fairness without sacrificing throughput.

8\. TaskGraph deserves to become an engine-wide scheduler

The current TaskGraph is already generic enough that it should not remain an AssetLib-only component. It would naturally serve shader compilation, navmesh generation, light baking, reflection capture generation, packaging, physics preprocessing, mesh optimization, and editor background work. Promoting it into a Core module would avoid multiple specialized schedulers throughout the engine.

9\. Drain-thread exception safety

dispatchCook() correctly converts cooker exceptions into CookResult objects, but the remainder of the drain phase (placeOutput(), commitResult(), filesystem operations, registry updates) is not wrapped by a similar exception boundary. An unexpected filesystem exception could terminate the entire drain thread. Making the complete commit path noexcept or catching all exceptions around drain callbacks would make the pipeline considerably more robust.

10\. Avoid converting UUIDs into strings

cookGraph() stores task indices in

unordered_map<std::string, int>

using uuid.toString() as the key. Since UUID already implements hashing, using unordered_map<UUID, int> would eliminate unnecessary string allocations and hashing overhead while making lookups simpler and faster.

11\. DDC scalability

The current DDC stores each blob as an individual filesystem object. This works extremely well initially but eventually leads to millions of files, which becomes inefficient on most filesystems. Many large content-addressable storage systems (Git, Perforce, Fossil, Bazel, etc.) eventually transition toward packed blob containers or packfiles. Planning for an optional packed backend would significantly improve scalability for very large projects.

12\. Worker protocol could expose richer diagnostics

The worker currently emits

RESULT
ERROR
OUTPUT
DEP

which is clean and easy to parse. However, extending the protocol with structured messages such as WARNING, LOG, and STAT would allow the editor to display useful information including peak memory usage, execution time, triangle counts, texture dimensions, compression ratios, or importer warnings without changing the existing protocol.

13\. Cycle reporting can be much more useful

When the task graph detects a dependency cycle, it currently lists the remaining unreachable nodes. While this correctly reports the problem, reconstructing and printing the actual dependency cycle (for example A → B → C → A) would make debugging dependency issues dramatically easier for users.

14\. Resource scheduling should evolve beyond RAM

Today the scheduler governs only memory usage. As the engine grows, tasks will compete for additional resources such as GPU encoders, disk bandwidth, network bandwidth, and CPU-intensive jobs. Allowing tasks to declare resource requirements (for example, "2 CPU cores, 1 GPU encoder, 500 MB RAM") would make the scheduler considerably more general without requiring architectural changes later.

15\. Cookers should eventually become transformation graphs

This is the architectural recommendation I consider the most important. At present, each cooker represents an entire asset transformation from source file to final runtime asset. As the engine matures, consider decomposing cookers into a directed graph of independent transformation stages such as parsing, validation, tangent generation, LOD generation, mesh optimization, meshlet generation, compression, packaging, and metadata generation. Each stage could become independently cacheable, parallelizable, inspectable, and reusable. This would effectively transform the cook pipeline into a fully incremental build system instead of a collection of monolithic importers.

16\. Overall memory safety assessment

I did not find any obvious ownership or lifetime bugs in the uploaded code. Memory ownership consistently relies on RAII (std::unique_ptr, std::vector, std::string, std::filesystem::path), and I did not observe patterns commonly associated with memory corruption such as raw owning pointers, manual new/delete, double frees, dangling references, or use-after-free errors. The primary risks I identified are related to future concurrency assumptions rather than memory management itself.