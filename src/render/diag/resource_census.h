#pragma once
// ── ResourceCensus — what is resident, who owns it, what leaked ─────────────
//
// ONE concern: accounting over a GpuResourceCache. Frame counters live in
// frame_gpu_stats.h; budget policy in gpu_budget.h.
//
// These three reports are the ones asked for — a VRAM census, a per-owner
// profile, a duplicate report and a leak detector — and they are all the same
// query against one table. They were impossible before the cache existed, not
// because nobody wrote them, but because "unused" had no definition without
// refcounts (renderer audit R1).
//
// Header-only and templated on the cache's handle type: the cache is
// payload-agnostic on purpose, so this stays testable with a fake handle and
// no GPU.
#include "render/gpu_resource_cache.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rdiag {

struct CensusRow {
    std::string key;      // content identity
    std::string owner;    // human-readable source (asset path, material name)
    uint64_t    bytes = 0;
    uint32_t    refs  = 0;
};

struct CensusReport {
    std::vector<CensusRow> rows;        // biggest first
    uint64_t totalBytes = 0;
    size_t   count      = 0;

    // Rows holding a reference. After teardown this should be EMPTY; anything
    // left is a leak, named and costed.
    std::vector<CensusRow> referenced() const {
        std::vector<CensusRow> out;
        for (const auto& r : rows) if (r.refs > 0) out.push_back(r);
        return out;
    }
};

// Bytes grouped by owner — the per-material / per-asset profile. Answers
// "which asset is costing me 32 MB", which a flat list does not.
struct OwnerCost {
    std::string owner;
    uint64_t    bytes     = 0;
    size_t      resources = 0;
};

// Two resident entries with different keys but identical size AND owner are
// the shape of a duplicate that content-keying failed to collapse. With the
// cache doing its job this is always empty, which makes it a REGRESSION TEST
// rather than a diagnostic — the interesting day is the day it is non-empty.
struct SuspectedDuplicate {
    std::string keyA, keyB, owner;
    uint64_t    bytes = 0;
};

template <typename Handle>
CensusReport census(const gpucache::GpuResourceCache<Handle>& cache) {
    CensusReport rep;
    for (const auto& e : cache.census()) {          // already sorted by size
        CensusRow r;
        r.key   = e.key;
        r.owner = e.owner;
        r.bytes = e.bytes;
        r.refs  = e.refs;
        rep.totalBytes += e.bytes;
        rep.rows.push_back(std::move(r));
    }
    rep.count = rep.rows.size();
    return rep;
}

inline std::vector<OwnerCost> byOwner(const CensusReport& rep) {
    std::unordered_map<std::string, OwnerCost> acc;
    for (const auto& r : rep.rows) {
        auto& o = acc[r.owner];
        o.owner  = r.owner;
        o.bytes += r.bytes;
        ++o.resources;
    }
    std::vector<OwnerCost> out;
    out.reserve(acc.size());
    for (auto& kv : acc) out.push_back(std::move(kv.second));
    std::sort(out.begin(), out.end(),
              [](const OwnerCost& a, const OwnerCost& b) { return a.bytes > b.bytes; });
    return out;
}

inline std::vector<SuspectedDuplicate> suspectedDuplicates(const CensusReport& rep) {
    std::vector<SuspectedDuplicate> out;
    for (size_t i = 0; i < rep.rows.size(); ++i)
        for (size_t j = i + 1; j < rep.rows.size(); ++j) {
            const auto& a = rep.rows[i];
            const auto& b = rep.rows[j];
            if (a.bytes == b.bytes && a.bytes > 0 && a.owner == b.owner
                    && a.key != b.key)
                out.push_back({a.key, b.key, a.owner, a.bytes});
        }
    return out;
}

// ── Leak detection ──────────────────────────────────────────────────────────
// Take a baseline, do work (load a scene, play, unload), compare. A clean
// cycle returns to baseline; a climb across repeated cycles is a leak. This is
// the mechanically checkable definition of "no render memory leaks", and it is
// what the soak lane should assert.
struct LeakBaseline {
    uint64_t bytes = 0;
    size_t   count = 0;
};

struct LeakResult {
    int64_t  bytesDelta = 0;
    int64_t  countDelta = 0;
    std::vector<CensusRow> stillReferenced;   // named + costed, if any
    bool clean() const {
        return bytesDelta <= 0 && countDelta <= 0 && stillReferenced.empty();
    }
};

template <typename Handle>
LeakBaseline takeBaseline(const gpucache::GpuResourceCache<Handle>& cache) {
    const auto rep = census(cache);
    return { rep.totalBytes, rep.count };
}

template <typename Handle>
LeakResult compareToBaseline(const gpucache::GpuResourceCache<Handle>& cache,
                             const LeakBaseline& base) {
    const auto rep = census(cache);
    LeakResult r;
    r.bytesDelta      = (int64_t)rep.totalBytes - (int64_t)base.bytes;
    r.countDelta      = (int64_t)rep.count      - (int64_t)base.count;
    r.stillReferenced = rep.referenced();
    return r;
}

} // namespace rdiag
