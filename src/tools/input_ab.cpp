// ── input_ab — SDL3 mouse vs the native hid backend, same hand movement ─────
//
// THE DECISION THIS SETTLES: can SDL3 be the raw-input provider for MOUSE and
// KEYBOARD, or only for gamepads? If it measures equal to IOHIDManager, the
// Win32 Raw Input and Linux evdev backends come off the roadmap entirely
// (backlog Phase F item 26). If it doesn't, SDL3 stays window+gamepad and the
// native backends get written.
//
// WHY BOTH IN ONE PROCESS: two separate runs would compare two different hand
// movements, which is worthless at this precision. This captures both streams
// simultaneously from the same physical motion.
//
// Build (needs SDL3 for one side and IOHIDManager for the other):
//   cmake -B build-ab -DENGINE_WINDOW_BACKEND=sdl3 -DHID_BACKEND=iohid
//   cmake --build build-ab --target input_ab && ./build-ab/input_ab
//
// WHAT IT CANNOT MEASURE: true end-to-end motion-to-photon latency needs
// external hardware (a photodiode on the display, or a logic analyser on the
// mouse). This measures the INPUT STACK only — event rate, timestamp quality,
// and how long an event sits before the app can see it. That is the part where
// the two backends actually differ.
#include "hid/hid.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Sample {
    uint64_t stampNs;     // the event's own timestamp (per-stream clock)
    uint64_t observedNs;  // when WE first saw it (same clock as stampNs)
    double   dx, dy;
};

struct Stats {
    size_t   count       = 0;
    double   perSec      = 0;
    double   medIntervalUs = 0, p99IntervalUs = 0;
    double   medLatencyUs = 0,  p99LatencyUs  = 0;
    size_t   distinctStamps = 0;
    size_t   maxPerStamp    = 0;
    double   absTravel      = 0;   // sum |dx|+|dy|
};

double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)std::min<double>(v.size() - 1,
                                              std::floor(p * (v.size() - 1)));
    return v[i];
}

Stats analyse(std::vector<Sample>& s, double seconds) {
    Stats st;
    st.count  = s.size();
    st.perSec = seconds > 0 ? (double)s.size() / seconds : 0.0;
    if (s.empty()) return st;

    std::sort(s.begin(), s.end(),
              [](const Sample& a, const Sample& b) { return a.stampNs < b.stampNs; });

    std::vector<double> gaps, lat;
    gaps.reserve(s.size()); lat.reserve(s.size());
    size_t run = 1, best = 1, distinct = 1;
    for (size_t i = 0; i < s.size(); ++i) {
        st.absTravel += std::fabs(s[i].dx) + std::fabs(s[i].dy);
        // Latency can only be measured within one clock; each stream uses its
        // own, which is why this is computed per-stream and never subtracted
        // across them.
        lat.push_back(s[i].observedNs >= s[i].stampNs
                      ? (double)(s[i].observedNs - s[i].stampNs) / 1000.0 : 0.0);
        if (i == 0) continue;
        const uint64_t d = s[i].stampNs - s[i - 1].stampNs;
        if (d == 0) { ++run; best = std::max(best, run); }
        else        { gaps.push_back((double)d / 1000.0); ++distinct; run = 1; }
    }
    st.distinctStamps = distinct;
    st.maxPerStamp    = std::max(best, run);
    st.medIntervalUs  = pct(gaps, 0.50);
    st.p99IntervalUs  = pct(gaps, 0.99);
    st.medLatencyUs   = pct(lat,  0.50);
    st.p99LatencyUs   = pct(lat,  0.99);
    return st;
}

void row(const char* label, const char* fmt, double a, double b) {
    char sa[64], sb[64];
    std::snprintf(sa, sizeof(sa), fmt, a);
    std::snprintf(sb, sizeof(sb), fmt, b);
    std::printf("  %-30s %14s %16s\n", label, sa, sb);
}

} // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10.0;

    // Measure SDL's RAW relative deltas, not OS-accelerated ones: the whole
    // point of a raw-input path is unaccelerated counts, and the comparison
    // against IOHIDManager (which is always raw) is only meaningful this way.
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("input_ab — move the mouse in me",
                                       640, 360, SDL_WINDOW_RESIZABLE);
    if (!win) {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // Relative mode: hidden, confined, delta-reporting — the state a game is in
    // during mouse-look, which is the case that matters.
    SDL_SetWindowRelativeMouseMode(win, true);
    SDL_RaiseWindow(win);

    hid::Context hidCtx;
    hid::Config cfg;             // mouse+keyboard+gamepad all on
    const bool hidOk = hidCtx.init(cfg);
    if (!hidOk) {
        std::printf("\n!! hid backend unavailable: %s\n", hidCtx.lastError());
        std::printf("   (on macOS this is usually Input Monitoring permission —\n"
                    "    System Settings > Privacy & Security > Input Monitoring)\n");
        std::printf("   Continuing with the SDL3 side only.\n");
    }

    // What did the native backend actually MATCH? Without this a run against a
    // device the backend does not claim looks identical to a broken backend.
    size_t nPointer = 0;
    if (hidOk) {
        hid::DeviceInfo devs[32];
        const size_t n = hidCtx.devices(devs, 32);
        std::printf("\nhid matched %zu device(s):\n", n);
        for (size_t i = 0; i < n; ++i) {
            const char* cls = "?";
            switch (devs[i].cls) {
                case hid::DeviceClass::Mouse:    cls = "Mouse";    ++nPointer; break;
                case hid::DeviceClass::Keyboard: cls = "Keyboard"; break;
                case hid::DeviceClass::Gamepad:  cls = "Gamepad";  break;
                default:                         cls = "Unknown";  break;
            }
            std::printf("   [%u] %-8s %04x:%04x  %s\n", devs[i].id, cls,
                        devs[i].vendorId, devs[i].productId, devs[i].name);
        }
        if (nPointer == 0) {
            std::printf(
                "\n   !! NO pointer-class device matched, so the hid column will\n"
                "      read 0 and the comparison is INVALID — not a backend bug.\n"
                "      hid_iohid matches GenericDesktop Mouse/Pointer usages; a\n"
                "      MacBook TRACKPAD is a Digitizer/TouchPad (page 0x0D) and\n"
                "      does not match. Plug in a real USB/wireless mouse and use\n"
                "      THAT for the sweep.\n");
        }
    }

    std::printf("\n=== input_ab — SDL3 vs %s ===\n",
                hidOk ? "IOHIDManager (hid)" : "hid UNAVAILABLE");
    std::printf("Capturing %.0fs. KEEP THE WINDOW FOCUSED and move the mouse\n"
                "continuously in wide strokes (both stacks see the same motion).\n\n",
                seconds);

    std::vector<Sample> sdlS, hidS;
    hid::Event evts[512];
    // Every hid event type seen, so "0 motion" can be told apart from
    // "0 events" — a backend delivering buttons but no motion is a different
    // problem from one delivering nothing.
    constexpr size_t kNumEventTypes = 8;
    size_t hidTypeHist[kNumEventTypes] = {};

    const uint64_t t0Sdl = SDL_GetTicksNS();
    const uint64_t t0Hid = hid::nowNs();
    bool quit = false;
    while (!quit) {
        const uint64_t nowSdl = SDL_GetTicksNS();
        if ((double)(nowSdl - t0Sdl) / 1e9 >= seconds) break;

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            const uint64_t obs = SDL_GetTicksNS();
            if (e.type == SDL_EVENT_MOUSE_MOTION) {
                // xrel/yrel: SDL's relative deltas. timestamp is stamped by SDL
                // at ingest with SDL_GetTicksNS (NOT a driver timestamp) — the
                // distinct-stamp metric below is what exposes what that costs.
                sdlS.push_back({ e.motion.timestamp, obs,
                                 (double)e.motion.xrel, (double)e.motion.yrel });
            } else if (e.type == SDL_EVENT_QUIT ||
                       (e.type == SDL_EVENT_KEY_DOWN &&
                        e.key.scancode == SDL_SCANCODE_ESCAPE)) {
                quit = true;
            }
        }

        if (hidOk) {
            const size_t n = hidCtx.drain(evts, 512);
            const uint64_t obs = hid::nowNs();
            for (size_t i = 0; i < n; ++i) {
                const size_t t = (size_t)evts[i].type;
                if (t < kNumEventTypes) ++hidTypeHist[t];
                if (evts[i].type == hid::EventType::MouseMotion)
                    hidS.push_back({ evts[i].timeNs, obs,
                                     (double)evts[i].value, (double)evts[i].value2 });
            }
        }
        SDL_Delay(1);        // ~1kHz sampling of both queues
    }

    const double elapsed = (double)(SDL_GetTicksNS() - t0Sdl) / 1e9;
    (void)t0Hid;

    Stats a = analyse(sdlS, elapsed);
    Stats b = analyse(hidS, elapsed);

    std::printf("  %-30s %14s %16s\n", "", "SDL3", "hid (IOHID)");
    std::printf("  %s\n", std::string(62, '-').c_str());
    row("motion events",            "%.0f",  (double)a.count,        (double)b.count);
    row("events / sec",             "%.0f",  a.perSec,               b.perSec);
    row("median interval (us)",     "%.1f",  a.medIntervalUs,        b.medIntervalUs);
    row("p99 interval (us)",        "%.1f",  a.p99IntervalUs,        b.p99IntervalUs);
    row("median observe lat (us)",  "%.1f",  a.medLatencyUs,         b.medLatencyUs);
    row("p99 observe lat (us)",     "%.1f",  a.p99LatencyUs,         b.p99LatencyUs);
    row("distinct timestamps",      "%.0f",  (double)a.distinctStamps,(double)b.distinctStamps);
    row("max events sharing stamp", "%.0f",  (double)a.maxPerStamp,  (double)b.maxPerStamp);
    row("total |dx|+|dy|",          "%.0f",  a.absTravel,            b.absTravel);
    if (hidOk) {
        std::printf("  dropped (hid ring): %llu\n",
                    (unsigned long long)hidCtx.dropped());
        static const char* kNames[kNumEventTypes] = {
            "None", "MouseMotion", "Scroll", "Button",
            "Key", "Axis", "DeviceAdded", "DeviceRemoved" };
        std::printf("  hid events by type:");
        size_t total = 0;
        for (size_t i = 0; i < kNumEventTypes; ++i) {
            total += hidTypeHist[i];
            if (hidTypeHist[i]) std::printf(" %s=%zu", kNames[i], hidTypeHist[i]);
        }
        if (total == 0) std::printf(" (none at all)");
        std::printf("\n");
    }

    // ── Verdict ────────────────────────────────────────────────────────────
    std::printf("\n  Interpretation\n");
    if (a.count == 0) {
        std::printf("  * NO SDL motion captured — was the window focused?\n");
    } else {
        const double stampRatio = (double)a.distinctStamps / (double)a.count;
        std::printf("  * SDL distinct-stamp ratio %.2f, max %zu events share one\n"
                    "    timestamp. Near 1.00 => usable for sub-tick input;\n"
                    "    well below => SDL batches stamps and sub-tick ordering\n"
                    "    inside a batch is lost.\n", stampRatio, a.maxPerStamp);
        if (b.count == 0 && hidOk && nPointer == 0) {
            std::printf("  * hid matched no pointer device, so NO comparison is\n"
                        "    possible from this run — see the warning above.\n");
        }
        if (b.count > 0) {
            std::printf("  * rate ratio SDL/hid %.2f. Far below 1.0 means SDL is\n"
                        "    COALESCING motion (fewer, fatter events).\n",
                        a.perSec / std::max(1.0, b.perSec));
            std::printf("  * travel ratio SDL/hid %.2f. ~1.0 means SDL is giving\n"
                        "    raw counts like IOHID; a different constant means it is\n"
                        "    scaled/accelerated (aim feel would not match).\n",
                        a.absTravel / std::max(1.0, b.absTravel));
        }
    }
    std::printf("\n  Reminder: end-to-end motion-to-photon needs external\n"
                "  hardware. This is the input stack only.\n\n");

    hidCtx.shutdown();
    SDL_SetWindowRelativeMouseMode(win, false);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
