#pragma once
#include "core/profiler.h"
#include "core/mem_counters.h"
#include <flecs.h>
#include <cstdio>

// ── MemoryChannel ───────────────────────────────────────────────────────────
// The profiler framework's first proof of extensibility: a new channel living
// in the RUNTIME layer (it reads flecs), registered with the core hub.
//
// It MEASURES allocations per frame — it never replaces an allocator:
//   • C++ new/delete  — core MemCounters (the counting global-new override)
//   • flecs (C malloc) — flecs's built-in ecs_os_api_*_count globals
//
// This is the safe, honest answer to "is malloc a per-frame problem, and from
// where?" — the question the whole zero-malloc temptation is really asking.
// Decide on pooling AFTER you can see the numbers, never before.
class MemoryChannel final : public prof::IProfilerChannel {
public:
    const char* channelName() const override { return "Memory"; }

    void beginFrame() override {
        auto& c = prof::memCounters();
        m_cppA0 = c.allocCount.load(std::memory_order_relaxed);
        m_cppF0 = c.freeCount.load(std::memory_order_relaxed);
        m_cppB0 = c.allocBytes.load(std::memory_order_relaxed);
        m_flM0  = ecs_os_api_malloc_count + ecs_os_api_calloc_count
                + ecs_os_api_realloc_count;
        m_flF0  = ecs_os_api_free_count;
    }
    void endFrame() override {
        auto& c = prof::memCounters();
        m_cppAllocs = c.allocCount.load(std::memory_order_relaxed) - m_cppA0;
        m_cppFrees  = c.freeCount.load(std::memory_order_relaxed)  - m_cppF0;
        m_cppBytes  = c.allocBytes.load(std::memory_order_relaxed) - m_cppB0;
        m_flAllocs  = (uint64_t)(ecs_os_api_malloc_count + ecs_os_api_calloc_count
                                 + ecs_os_api_realloc_count) - m_flM0;
        m_flFrees   = (uint64_t)ecs_os_api_free_count - m_flF0;
    }

    // ── Read API (overlay / dump) ───────────────────────────────────────────
    uint64_t cppAllocs() const { return m_cppAllocs; }
    uint64_t cppFrees()  const { return m_cppFrees; }
    uint64_t cppBytes()  const { return m_cppBytes; }
    uint64_t flecsAllocs() const { return m_flAllocs; }
    uint64_t flecsFrees()  const { return m_flFrees; }

    void logLastFrame(const char* tag) const {
        std::printf("[Memory] %s — C++ new:%llu free:%llu (%llu B) | "
                    "flecs alloc:%llu free:%llu\n", tag,
                    (unsigned long long)m_cppAllocs, (unsigned long long)m_cppFrees,
                    (unsigned long long)m_cppBytes,
                    (unsigned long long)m_flAllocs, (unsigned long long)m_flFrees);
        std::fflush(stdout);
    }

private:
    uint64_t m_cppA0 = 0, m_cppF0 = 0, m_cppB0 = 0, m_flM0 = 0, m_flF0 = 0;
    uint64_t m_cppAllocs = 0, m_cppFrees = 0, m_cppBytes = 0,
             m_flAllocs = 0, m_flFrees = 0;
};
