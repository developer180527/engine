// ── CookPipeline — cook ORCHESTRATION ────────────────────────────────────────
// What needs cooking, in what order, and what the registry records after.
// The mechanics live behind their own seams:
//   cook_key.h        identity + staleness (DDC keys)
//   cook_dispatch.h   execution mode (isolated child process / in-process)
//   ddc_manifest.h    cached-output record format (manifest of member blobs)
//   ddc.h             the content-addressed two-tier store
//   task_graph.h      cost-weighted DAG scheduler + thermal governance
#include "assetlib/cook_pipeline.h"
#include "assetlib/ddc_manifest.h"
#include "assetlib/task_graph.h"
#include "cook_dispatch.h"
#include "cook_key.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <unordered_map>

namespace assetlib {

CookPipeline::CookPipeline(AssetRegistry& registry,
                           std::filesystem::path projectRoot,
                           std::filesystem::path cacheRoot)
    : m_registry(registry)
    , m_projectRoot(std::move(projectRoot))
    , m_cacheRoot(std::move(cacheRoot))
    , m_ddc() {}   // roots from ENGINE_DDC / ENGINE_DDC_SHARED (or defaults)

// ── Cooker registry ──────────────────────────────────────────────────────────

void CookPipeline::registerCooker(std::unique_ptr<ICooker> cooker) {
    m_cookers.push_back(std::move(cooker));
}

ICooker* CookPipeline::findCooker(const std::string& ext) const {
    for (auto& c : m_cookers)
        for (auto& e : c->extensions())
            if (e == ext) return c.get();
    return nullptr;
}

bool CookPipeline::hasCookerFor(const std::string& ext) const {
    std::string lower = ext;
    for (auto& c : lower) c = static_cast<char>(std::tolower(c));
    return findCooker(lower) != nullptr;
}

// ── Identity + staleness (policy in cook_key.h) ──────────────────────────────

// Dependency source hashes for one record: from the batch snapshot when the
// caller built one, else a single query. A loop over every asset must pass the
// index — a query per asset is the O(N)-prepares mistake removed from scan().
std::vector<std::string>
CookPipeline::depHashesFor(const AssetRecord& rec,
                           const DepHashIndex* idx) const {
    if (!idx) return m_registry.dependencySourceHashes(rec.uuid);
    const auto it = idx->find(rec.uuid.toString());
    return it == idx->end() ? std::vector<std::string>{} : it->second;
}

CookPipeline::DepHashIndexPublic CookPipeline::dependencyHashIndex() const {
    return m_registry.allDependencySourceHashes();
}

std::string CookPipeline::currentKey(const AssetRecord& rec,
                                     ICooker* cooker) const {
    if (!cooker) return {};
    return computeCookKey(rec, *cooker, m_projectRoot,
                          depHashesFor(rec, nullptr));
}

static bool isStaleImpl(const AssetRecord& rec, ICooker* cooker,
                        const std::filesystem::path& projectRoot,
                        const std::filesystem::path& cacheRoot,
                        const std::vector<std::string>& depHashes) {
    if (!cooker) return false;              // nothing can cook it
    return cookIsStale(rec, computeCookKey(rec, *cooker, projectRoot, depHashes),
                       cacheRoot);
}

bool CookPipeline::isStale(const AssetRecord& rec) const {
    ICooker* cooker = findCooker(lowerExtOf(rec.sourcePath));
    return isStaleImpl(rec, cooker, m_projectRoot, m_cacheRoot,
                       depHashesFor(rec, nullptr));
}

bool CookPipeline::isStaleWith(const AssetRecord& rec,
                               const DepHashIndexPublic& idx) const {
    ICooker* cooker = findCooker(lowerExtOf(rec.sourcePath));
    return isStaleImpl(rec, cooker, m_projectRoot, m_cacheRoot,
                       depHashesFor(rec, &idx));
}

// ── Per-record resolution + output placement ─────────────────────────────────

std::optional<CookPipeline::Resolved>
CookPipeline::resolve(const AssetRecord& rec, const DepHashIndex* idx) const {
    Resolved r;
    r.cooker = findCooker(lowerExtOf(rec.sourcePath));
    if (!r.cooker) return std::nullopt;
    r.key = computeCookKey(rec, *r.cooker, m_projectRoot,
                           depHashesFor(rec, idx));
    if (r.key.empty()) return std::nullopt;     // unreadable source

    const auto outDir = m_cacheRoot / (assetTypeName(rec.type) + "s");
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    r.sourceRel  = rec.sourcePath;
    r.sourcePath = m_projectRoot / rec.sourcePath;
    r.outPath    = outDir / (rec.uuid.toString() + ".cooked");
    r.tmpPath    = outDir / (rec.uuid.toString() + ".cooking");
    return r;
}

void CookPipeline::placeOutput(CookResult& res, const std::string& key,
                               const std::filesystem::path& tmpPath,
                               const std::filesystem::path& outPath,
                               const std::vector<std::filesystem::path>& extras) {
    std::error_code ec;
    if (!res.success) {
        std::filesystem::remove(tmpPath, ec);
        return;
    }
    // Ingest into the DDC, then materialize FROM the store — a cooker must
    // never write the final path directly, because materialization hardlinks
    // blobs and an ofstream on a hardlinked output would truncate the blob
    // for every project sharing it.
    if (ddcStoreRecord(m_ddc, key, tmpPath, extras)
            && ddcFetchRecord(m_ddc, key, outPath)) {
        std::filesystem::remove(tmpPath, ec);
        return;
    }
    // Store unusable (disk full?) — keep the correct output anyway.
    std::filesystem::rename(tmpPath, outPath, ec);
    if (ec)
        res = { .success=false,
                .error="cannot place cooked output: " + ec.message() };
}

bool CookPipeline::backfillDdc(const AssetRecord& rec) {
    // Only records whose output is genuinely current and present. A skipped
    // cook (empty cookedPath) has nothing to store; a Failed one must not be
    // cached as if it were output.
    if (rec.state != AssetState::Ready || rec.cookedPath.empty()) return false;

    ICooker* cooker = findCooker(lowerExtOf(rec.sourcePath));
    if (!cooker) return false;
    const std::string key = computeCookKey(rec, *cooker, m_projectRoot);
    // An empty key means the source is unreadable — no identity, nothing to
    // address. A key that disagrees with the record is a stale record, which
    // is the cook path's business, not ours.
    if (key.empty() || key != rec.ddcKey) return false;

    if (m_ddc.contains(key)) return false;          // already cached — the norm

    const auto primary = m_cacheRoot / rec.cookedPath;
    std::error_code ec;
    if (!std::filesystem::exists(primary, ec)) return false;

    // The cooker re-derives its own sibling set; guessing by glob would sweep
    // up stale siblings from an older version (see ICooker::enumerateOutputs).
    std::vector<std::filesystem::path> extras;
    cooker->enumerateOutputs(primary, extras);
    for (const auto& e : extras)
        if (!std::filesystem::exists(e, ec)) return false;   // incomplete set

    return ddcStoreRecord(m_ddc, key, primary, extras);
}

// ── Registry commit ──────────────────────────────────────────────────────────

void CookPipeline::commitResult(const UUID& uuid, const CookResult& res,
                                const std::string& key, uint32_t cookerVersion,
                                const std::filesystem::path& outPath,
                                const std::vector<UUID>& deps) {
    // A cancelled cook is not a verdict on the asset — record NOTHING. Writing
    // Failed with this key would make the record look "already attempted at
    // these exact inputs", and staleness would skip it forever (see
    // CookResult::cancelled). Leaving it untouched keeps it stale → retried.
    if (res.cancelled) return;

    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return;

    if (res.success) {
        for (auto& dep : deps) m_registry.addDependency(uuid, dep);
        // error_code overload: the throwing one would propagate out of the
        // graph's drain lane on a filesystem hiccup mid-commit.
        std::error_code relEc;
        rec->cookedPath  = std::filesystem::relative(outPath, m_cacheRoot, relEc).string();
        rec->state       = AssetState::Ready;
        rec->errorMessage.clear();
    } else {
        // Skip or failure: delete any stale .cooked binary so no code path
        // can accidentally serve it.
        if (!rec->cookedPath.empty()) {
            std::error_code ec;
            std::filesystem::remove(m_cacheRoot / rec->cookedPath, ec);
        }
        rec->cookedPath.clear();
        if (res.skipped) {
            // Cooker can't handle this asset type (e.g. skinned meshes) —
            // Ready with empty cookedPath; the runtime import path serves it.
            rec->state        = AssetState::Ready;
            rec->errorMessage = res.error;   // "why it was skipped"
        } else {
            rec->state        = AssetState::Failed;
            rec->errorMessage = res.error.empty()
                ? "cook failed (no error reported)" : res.error;
        }
    }
    rec->ddcKey      = key;                  // the attempt is now addressed
    rec->cookVersion = cookerVersion;
    rec->cookedAt    = static_cast<int64_t>(std::time(nullptr));
    m_registry.update(*rec);
}

// ── Single-asset cook ────────────────────────────────────────────────────────

CookResult CookPipeline::cookOne(const UUID& uuid) {
    return cookInternal(uuid, /*useFetch=*/true);
}

CookResult CookPipeline::cookInternal(const UUID& uuid, bool useFetch) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found in registry" };
    if (!isStale(*rec)) return { .success=true }; // already fresh

    auto r = resolve(*rec);
    if (!r) {
        const std::string ext = lowerExtOf(rec->sourcePath);
        return { .success=false,
                 .error=findCooker(ext) ? "source unreadable (no content hash)"
                        : "No cooker registered for extension: " + ext };
    }

    // Cache hit: someone (this machine, a teammate via the shared tier)
    // already cooked these exact inputs — materialize, done.
    if (useFetch && ddcFetchRecord(m_ddc, r->key, r->outPath)) {
        commitResult(uuid, { .success=true }, r->key, r->cooker->version(),
                     r->outPath, {});
        std::printf("[AssetLib] DDC hit: %s\n", r->sourceRel.c_str());
        return { .success=true, .cookedPath=r->outPath.string() };
    }

    CookContext ctx;
    ctx.uuid       = uuid;
    ctx.sourcePath = r->sourcePath;
    ctx.outputPath = r->tmpPath;
    std::vector<UUID> deps;
    std::vector<std::filesystem::path> extras;
    ctx.addDependency = [&deps](const UUID& dep) { deps.push_back(dep); };
    ctx.addOutput     = [&extras](const std::filesystem::path& p) { extras.push_back(p); };

    auto result = dispatchCook(m_workerExe, *r->cooker, ctx);
    placeOutput(result, r->key, r->tmpPath, r->outPath, extras);


    commitResult(uuid, result, r->key, r->cooker->version(), r->outPath, deps);
    if (result.success) {
        result.cookedPath = r->outPath.string();
        std::printf("[AssetLib] Cooked: %s\n", r->sourceRel.c_str());
    }
    return result;
}

CookResult CookPipeline::forceRecook(const UUID& uuid) {
    auto rec = m_registry.findByUUID(uuid);
    if (!rec) return { .success=false, .error="UUID not found" };
    // A force-recook exists because someone suspects the cached output —
    // evict the local blob and cook with the DDC read path bypassed, so we
    // genuinely re-cook instead of re-fetching the very bytes under
    // suspicion. (Shared tier untouched: other machines may be serving from
    // it; ingest there is first-writer-wins, so a poisoned shared blob needs
    // an admin wipe — same as every production DDC.)
    if (ICooker* cooker = findCooker(lowerExtOf(rec->sourcePath)))
        m_ddc.evictLocal(computeCookKey(*rec, *cooker, m_projectRoot,
                                        depHashesFor(*rec, nullptr)));
    rec->ddcKey.clear();                    // force staleness
    m_registry.update(*rec);
    return cookInternal(uuid, /*useFetch=*/false);
}

// ── Batch cooks ──────────────────────────────────────────────────────────────

int CookPipeline::cookAll(std::function<void(int,int)> progress) {
    auto all   = m_registry.all();
    int  total = static_cast<int>(all.size());

    // One query for every asset's dependency hashes, not one per asset.
    const DepHashIndex depIdx = m_registry.allDependencySourceHashes();

    std::vector<UUID> stale;
    int backfilled = 0;
    for (auto& rec : all) {
        if (isStaleWith(rec, depIdx)) { stale.push_back(rec.uuid); continue; }
        if (backfillDdc(rec)) ++backfilled;         // warm .cache, cold DDC
        if (rec.state != AssetState::Ready
                && rec.state != AssetState::Failed) {   // fresh but unmarked
            auto r = m_registry.findByUUID(rec.uuid);
            if (r) { r->state = AssetState::Ready; m_registry.update(*r); }
        }
    }
    if (backfilled > 0)
        std::printf("[AssetLib] DDC: back-filled %d up-to-date asset(s)\n",
                    backfilled);

    std::atomic<int> done{ total - static_cast<int>(stale.size()) };
    int cooked = cookMany(stale, [&](const std::string&, bool) {
        if (progress) progress(done.fetch_add(1) + 1, total);
    });
    if (progress) progress(total, total);
    return cooked;
}

int CookPipeline::cookGraph(const std::vector<UUID>& uuids,
                            std::vector<ExtraTask> extras,
                            std::function<void(const std::string&, bool)> onResult,
                            std::function<bool()> shouldContinue) {
    struct Work {
        UUID                  uuid;
        Resolved              r;
        std::vector<UUID>     deps;
        std::vector<std::filesystem::path> outputs;
        CookResult            result;
    };

    // ── Phase 1 (caller thread): resolve records into self-contained work,
    // serving DDC hits inline — a hit is a hardlink + a registry row, there
    // is nothing to parallelize.
    std::vector<Work> work;
    work.reserve(uuids.size());
    const DepHashIndex depIdx = m_registry.allDependencySourceHashes();
    int hits = 0, backfilled = 0;
    for (const auto& uuid : uuids) {
        auto rec = m_registry.findByUUID(uuid);
        if (!rec) continue;
        if (!isStaleWith(*rec, depIdx)) {
            // Up to date. If the store somehow lacks it (wiped ~/.engine, a
            // tree copied from another machine), ingest it now rather than
            // paying a full cook at the next .cache wipe.
            if (backfillDdc(*rec)) ++backfilled;
            continue;
        }
        auto r = resolve(*rec, &depIdx);
        if (!r) continue;                   // no cooker / unreadable source

        if (ddcFetchRecord(m_ddc, r->key, r->outPath)) {
            commitResult(uuid, { .success=true }, r->key, r->cooker->version(),
                         r->outPath, {});
            ++hits;
            if (onResult) onResult(r->sourceRel, true);
            continue;
        }
        work.push_back(Work{ .uuid = uuid, .r = std::move(*r) });
    }
    const int numWork = static_cast<int>(work.size());
    if (hits > 0)
        std::printf("[AssetLib] DDC: %d asset(s) restored from cache\n", hits);
    if (backfilled > 0)
        std::printf("[AssetLib] DDC: back-filled %d up-to-date asset(s)\n",
                    backfilled);
    if (numWork == 0 && extras.empty()) return hits;

    // ── Phase 2: build the task graph ──────────────────────────────────────
    // Every miss is a node: work() = cook + DDC ingest (worker pool, memory-
    // governed, QoS-demoted — TaskGraph owns the thermal levers); done() =
    // registry commit + progress (drain lane = this thread, so the single
    // registry connection is never shared). Nodes are cost-weighted by the
    // cooker's estimate — the graph dispatches longest-first, so the 8K
    // texture starts at t=0 instead of straggling behind a hundred trinkets.
    // NOTE: `work` is fully sized above and must not reallocate now that
    // lambdas capture references into it.
    TaskGraph graph;
    int cooked = 0, cancelled = 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::unordered_map<UUID, int> nodeByUuid;   // UUID hashes directly

    // Cancellation reaches the COOKS, not just the dispatcher: workers poll
    // this and SIGKILL their child, so quitting the editor doesn't wait out a
    // multi-minute bake. Must be thread-safe — CookService's reads an atomic.
    CancelFn isCancelled;
    if (shouldContinue)
        isCancelled = [&shouldContinue] { return !shouldContinue(); };

    for (auto& w : work) {
        CookContext estCtx;                 // estimate may peek the header
        estCtx.uuid       = w.uuid;
        estCtx.sourcePath = w.r.sourcePath;
        const size_t est  = w.r.cooker->estimatePeakBytes(estCtx);

        const int node = graph.add("asset:" + w.r.sourceRel, est,
            /*work — pool*/ [this, &w, &isCancelled] {
                CookContext ctx;
                ctx.uuid          = w.uuid;
                ctx.sourcePath    = w.r.sourcePath;
                ctx.outputPath    = w.r.tmpPath;   // never the final path
                ctx.addDependency = [&w](const UUID& dep) { w.deps.push_back(dep); };
                ctx.addOutput     = [&w](const std::filesystem::path& p) {
                    w.outputs.push_back(p);
                };
                w.result = dispatchCook(m_workerExe, *w.r.cooker, ctx, isCancelled);
                // DDC ingest on the pool too — hashing/copying the blobs of a
                // big mesh is real work the drain lane shouldn't serialize.
                placeOutput(w.result, w.r.key, w.r.tmpPath, w.r.outPath,
                            w.outputs);
            },
            /*done — drain*/ [this, &w, &cooked, &cancelled, &onResult] {
                // Cancelled: commit nothing, count nothing, report nothing —
                // the asset stays stale and cooks on the next pass. Reporting
                // it as a failure would just spam the shutdown log.
                if (w.result.cancelled) { ++cancelled; return; }
                if (!w.result.success && !w.result.skipped)
                    std::printf("[AssetLib] Cook FAILED: %s — %s\n",
                                w.r.sourceRel.c_str(), w.result.error.c_str());
                commitResult(w.uuid, w.result, w.r.key, w.r.cooker->version(),
                             w.r.outPath, w.deps);
                if (w.result.success) ++cooked;
                if (onResult)
                    onResult(w.r.sourceRel, w.result.success || w.result.skipped);
            });
        nodeByUuid.emplace(w.uuid, node);
    }

    // Dependency edges among the cook set (registry graph). Sparse today —
    // meshes cook their textures inline — but any cooker that READS another
    // asset's cooked output is ordered correctly from here on.
    for (auto& w : work) {
        const int self = nodeByUuid.at(w.uuid);
        for (const auto& dep : m_registry.dependencies(w.uuid)) {
            const auto dit = nodeByUuid.find(dep);
            if (dit != nodeByUuid.end() && dit->second != self)
                graph.addEdge(dit->second, self);
        }
    }

    // Extra tasks (scene cooks): run after their own referenced assets are
    // cooked AND committed — and immediately when none of them are cooking.
    std::vector<char> extraOk(extras.size(), 0);
    for (size_t i = 0; i < extras.size(); ++i) {
        ExtraTask& e  = extras[i];
        char&      ok = extraOk[i];
        const int node = graph.add("extra:" + e.name, e.estBytes,
            [&e, &ok] { ok = (e.run && e.run()) ? 1 : 0; },
            [&e, &ok] { if (e.onDone) e.onDone(ok != 0); });
        for (const auto& u : e.waitFor) {
            const auto dit = nodeByUuid.find(u);
            if (dit != nodeByUuid.end()) graph.addEdge(dit->second, node);
        }
    }

    TaskGraph::Options opts;
    opts.shouldContinue = shouldContinue;   // graph keeps its own copy
    graph.run(opts);

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    std::printf("[AssetLib] Cooked %d/%d asset(s), %zu extra task(s) in %.1f ms "
                "(+%d from DDC)\n", cooked, numWork, extras.size(), ms, hits);
    if (cancelled > 0)
        std::printf("[AssetLib] %d cook(s) cancelled — left stale, will retry\n",
                    cancelled);
    return cooked + hits;
}

} // namespace assetlib
