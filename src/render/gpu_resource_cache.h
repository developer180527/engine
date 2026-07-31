#pragma once
// ── GpuResourceCache — identity, refcounts and a byte budget for GPU data ────
//
// Phase 1 of docs/renderer-audit-and-plan.md, and the foundation the render
// tooling needs. Finding R1 of that audit: GPU resources have no identity, no
// refcount and no dedup. `TextureRegistry::addTexture` is a slot allocator —
// every call makes a new GPU texture, so two materials naming the same image
// get two copies of it in VRAM, and nothing can say when either is free.
//
// Three consequences, all fixed here:
//   • DUPLICATES are prevented structurally. Resources are keyed by CONTENT,
//     so asking twice returns the same handle. Dedup is not a cleanup pass
//     that runs later; a duplicate cannot be created in the first place.
//   • LEAKS become definable. "Unused" means refs == 0, which is a fact rather
//     than a guess, so a leak detector can assert on it.
//   • TOOLING becomes a query. A VRAM census, a per-material profile and a
//     duplicate report are all reads of this one table. They were impossible
//     before not because nobody wrote them, but because the data did not exist.
//
// ── Design notes ────────────────────────────────────────────────────────────
//
// SITS ON TOP OF the existing registries rather than replacing them. On a miss
// the caller's factory does the real create + registry insert; the cache only
// remembers the mapping. That keeps this change small and keeps the registries'
// RAII destruction exactly as it is.
//
// PAYLOAD-AGNOSTIC on purpose. The cache never mentions bgfx, so all of its
// logic — dedup, refcounting, LRU, budget — is unit-testable with a fake
// handle type and no GPU at all. `src/render` has no GPU test harness; this
// component does not need one.
//
// KEY BY CONTENT HASH, NOT PATH. Two files with identical bytes (a texture
// copied into two asset packs) must share one upload, and one file reachable
// by two paths must not upload twice. The DDC already computes exactly this
// hash during the cook, so the key is free.
//
// refs == 0 MEANS EVICTABLE, NOT DEAD. This is a cache, not a unique_ptr:
// dropping the last reference keeps the resource resident so a reload is free,
// and it is only actually released under budget pressure, oldest-unused first.
#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gpucache {

// One resident resource, as the tools see it.
template <typename Handle>
struct ResourceInfo {
    std::string key;        // content hash (or a stable id) — the identity
    std::string owner;      // human-readable source, for the census/profile
    Handle      handle{};
    size_t      bytes = 0;
    uint32_t    refs  = 0;
    uint64_t    lastUse = 0;
};

struct CacheStats {
    size_t   liveCount   = 0;
    size_t   liveBytes   = 0;
    size_t   peakBytes   = 0;
    uint64_t hits        = 0;   // dedup wins: an upload that did NOT happen
    uint64_t misses      = 0;   // real creates
    uint64_t evictions   = 0;
    size_t   budgetBytes = 0;
};

// `Handle` is any copyable handle type (TextureHandle, MeshHandle, or an int
// in tests). `Destroy` is invoked when an entry is actually released.
template <typename Handle>
class GpuResourceCache {
public:
    using Factory   = std::function<bool(Handle& outHandle, size_t& outBytes)>;
    using Destroyer = std::function<void(const Handle&)>;

    explicit GpuResourceCache(Destroyer destroyer = {}, size_t budgetBytes = 0)
        : m_destroy(std::move(destroyer)), m_budget(budgetBytes) {}

    // 0 = unbounded. Set from the project's VRAM budget; on the Intel UHD 630
    // target the whole graphics budget is 128 MB, so this is a real limit and
    // not a formality.
    void setBudget(size_t bytes) { m_budget = bytes; }
    size_t budget() const        { return m_budget; }

    // THE core operation. On a hit: bump the refcount, touch LRU, return the
    // existing handle — no upload. On a miss: run `factory`, which must create
    // the resource and report its byte size.
    //
    // Returns false only when the factory fails; `out` is then untouched.
    bool acquire(const std::string& key, const std::string& owner,
                 const Factory& factory, Handle& out) {
        auto it = m_entries.find(key);
        if (it != m_entries.end()) {
            ++it->second.refs;
            it->second.lastUse = ++m_clock;
            ++m_stats.hits;
            // What the old slot-allocator behaviour WOULD have uploaded here.
            // Tracked so the dedup win is a measured number, not a claim.
            m_savedBytes += it->second.bytes;
            out = it->second.handle;
            return true;
        }

        Handle h{};
        size_t bytes = 0;
        if (!factory(h, bytes)) return false;   // caller reports its own error

        ResourceInfo<Handle> info;
        info.key     = key;
        info.owner   = owner;
        info.handle  = h;
        info.bytes   = bytes;
        info.refs    = 1;
        info.lastUse = ++m_clock;
        m_entries.emplace(key, std::move(info));

        ++m_stats.misses;
        m_liveBytes += bytes;
        if (m_liveBytes > m_stats.peakBytes) m_stats.peakBytes = m_liveBytes;
        out = h;
        return true;
    }

    // Drop one reference. The resource STAYS resident (see the design note) —
    // eviction is a budget decision, made in evictOverBudget().
    // Returns false if the key is unknown or already at zero, which is a
    // double-release and worth surfacing rather than silently underflowing.
    bool release(const std::string& key) {
        auto it = m_entries.find(key);
        if (it == m_entries.end() || it->second.refs == 0) return false;
        --it->second.refs;
        return true;
    }

    void addRef(const std::string& key) {
        auto it = m_entries.find(key);
        if (it != m_entries.end()) { ++it->second.refs; it->second.lastUse = ++m_clock; }
    }

    bool contains(const std::string& key) const {
        return m_entries.find(key) != m_entries.end();
    }
    uint32_t refCount(const std::string& key) const {
        auto it = m_entries.find(key);
        return it == m_entries.end() ? 0u : it->second.refs;
    }

    // Release unreferenced resources, least-recently-used first, until the
    // budget is met. NEVER touches a referenced entry: evicting something a
    // draw call still points at is a use-after-free, so exceeding the budget
    // is strictly preferable and is reported instead (see overBudget()).
    size_t evictOverBudget() {
        if (m_budget == 0 || m_liveBytes <= m_budget) return 0;

        std::vector<const ResourceInfo<Handle>*> victims;
        victims.reserve(m_entries.size());
        for (const auto& kv : m_entries)
            if (kv.second.refs == 0) victims.push_back(&kv.second);

        std::sort(victims.begin(), victims.end(),
                  [](const auto* a, const auto* b) { return a->lastUse < b->lastUse; });

        size_t freed = 0;
        for (const auto* v : victims) {
            if (m_liveBytes <= m_budget) break;
            const std::string key = v->key;      // copy: erase invalidates v
            freed += evictEntry(key);
        }
        return freed;
    }

    // Force-release one unreferenced resource. Returns bytes freed (0 if it is
    // unknown or still referenced).
    size_t evict(const std::string& key) {
        auto it = m_entries.find(key);
        if (it == m_entries.end() || it->second.refs != 0) return 0;
        return evictEntry(key);
    }

    // Release everything unreferenced. Used at scene teardown, and the basis
    // of the leak check: after a full unload this should bring liveBytes back
    // to the baseline, and whatever remains is still referenced by someone.
    size_t evictAllUnreferenced() {
        std::vector<std::string> keys;
        keys.reserve(m_entries.size());
        for (const auto& kv : m_entries)
            if (kv.second.refs == 0) keys.push_back(kv.first);
        size_t freed = 0;
        for (const auto& k : keys) freed += evictEntry(k);
        return freed;
    }

    // ── Tooling queries (Phase 2 of the plan reads these) ───────────────────

    // Everything resident, biggest first — the VRAM census, and the
    // per-material profile once owners name materials.
    std::vector<ResourceInfo<Handle>> census() const {
        std::vector<ResourceInfo<Handle>> out;
        out.reserve(m_entries.size());
        for (const auto& kv : m_entries) out.push_back(kv.second);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
        return out;
    }

    // Still-referenced resources — a LEAK REPORT when called after teardown,
    // where the expected answer is "nothing". Note this can only ever be
    // written because refs exist; it is the payoff for Phase 1.
    std::vector<ResourceInfo<Handle>> stillReferenced() const {
        std::vector<ResourceInfo<Handle>> out;
        for (const auto& kv : m_entries)
            if (kv.second.refs > 0) out.push_back(kv.second);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
        return out;
    }

    bool overBudget() const { return m_budget != 0 && m_liveBytes > m_budget; }

    CacheStats stats() const {
        CacheStats s = m_stats;
        s.liveCount   = m_entries.size();
        s.liveBytes   = m_liveBytes;
        s.budgetBytes = m_budget;
        return s;
    }

    // Bytes an equivalent non-deduplicating registry would have uploaded, i.e.
    // what the OLD behaviour cost. hits * (bytes of the hit entry) is the
    // saving; reported so the win is measurable rather than asserted.
    size_t dedupSavedBytes() const { return m_savedBytes; }

private:
    size_t evictEntry(const std::string& key) {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) return 0;
        const size_t bytes = it->second.bytes;
        if (m_destroy) m_destroy(it->second.handle);
        m_entries.erase(it);
        m_liveBytes -= bytes;
        ++m_stats.evictions;
        return bytes;
    }

    std::unordered_map<std::string, ResourceInfo<Handle>> m_entries;
    Destroyer  m_destroy;
    size_t     m_budget      = 0;
    size_t     m_liveBytes   = 0;
    size_t     m_savedBytes  = 0;
    uint64_t   m_clock       = 0;   // LRU stamp
    CacheStats m_stats{};
};

} // namespace gpucache
