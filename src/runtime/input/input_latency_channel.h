#pragma once
// ── InputLatencyChannel — the LatencyTracker (profiler channel) ──────────────
// Reads the InputManager's per-frame latency accumulators into the profiler
// frame, same pattern as MemoryChannel. Three numbers that matter:
//   queue   device -> pump      (how long events sat before the engine saw
//                                them; hardware timestamps on the raw path)
//   tick    device -> snapshot  (oldest event folded into this sim tick)
//   look    device -> camera    (newest motion count vs consumeLook time —
//                                the late-latch aim latency, the felt one)
// Window-source events are stamped at pump, so cooked-path numbers read ~0;
// the interesting data comes from raw-hid runs. Input-to-PHOTON needs the
// frame-pacing work (Phase E) to pair these with present timestamps.
#include <cstdio>

#include "core/profiler.h"
#include "runtime/input/input_manager.h"

class InputLatencyChannel final : public prof::IProfilerChannel {
public:
    explicit InputLatencyChannel(input::InputManager* m) : m_input(m) {}
    const char* channelName() const override { return "InputLatency"; }

    void beginFrame() override {}
    void endFrame() override {
        if (m_input) m_last = m_input->takeLatencyStats();
    }

    const input::InputManager::LatencyStats& last() const { return m_last; }

    void logLastFrame(const char* tag) const {
        if (!m_last.events) {
            std::printf("[InputLat] %s — idle (no events this frame)\n", tag);
            return;
        }
        std::printf("[InputLat] %s — %llu ev | queue avg %.2fms max %.2fms | "
                    "tick lag %.2fms | look lag %.2fms\n",
                    tag, (unsigned long long)m_last.events,
                    m_last.queueSumNs / (double)m_last.events / 1e6,
                    m_last.queueMaxNs / 1e6,
                    m_last.tickLagNs / 1e6,
                    m_last.lookLagNs / 1e6);
        std::fflush(stdout);
    }

private:
    input::InputManager*             m_input = nullptr;
    input::InputManager::LatencyStats m_last;
};
