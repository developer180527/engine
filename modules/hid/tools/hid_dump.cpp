// ── hid_dump — standalone verifier for the hid module ───────────────────────
//   hid_dump --list           enumerate matching devices and exit
//   hid_dump [--seconds N]    capture N seconds (default 8):
//                             non-motion events print as lines; motion is
//                             aggregated per second (reports/s ≈ the mouse's
//                             real polling rate reaching us raw).
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "hid/hid.h"

static std::atomic<bool> g_stop{false};

static const char* clsName(hid::DeviceClass c) {
    switch (c) {
        case hid::DeviceClass::Mouse:    return "Mouse";
        case hid::DeviceClass::Keyboard: return "Keyboard";
        case hid::DeviceClass::Gamepad:  return "Gamepad";
        default:                         return "Unknown";
    }
}

// Tiny sanity-check names for common HID keyboard usages (tool-only sugar —
// the module itself never names keys).
static const char* keyName(uint16_t usage) {
    static char buf[8];
    if (usage >= 0x04 && usage <= 0x1D) {           // A..Z
        std::snprintf(buf, sizeof(buf), "%c", 'A' + (usage - 0x04));
        return buf;
    }
    if (usage >= 0x1E && usage <= 0x26) {           // 1..9
        std::snprintf(buf, sizeof(buf), "%c", '1' + (usage - 0x1E));
        return buf;
    }
    switch (usage) {
        case 0x27: return "0";
        case 0x28: return "Enter";  case 0x29: return "Esc";
        case 0x2C: return "Space";  case 0xE0: return "LCtrl";
        case 0xE1: return "LShift"; case 0xE2: return "LAlt";
        case 0xE3: return "LCmd";
        default:
            std::snprintf(buf, sizeof(buf), "0x%02X", usage);
            return buf;
    }
}

static void printDevices(hid::Context& ctx) {
    hid::DeviceInfo devs[32];
    const size_t n = ctx.devices(devs, 32);
    std::printf("%zu device(s):\n", n);
    for (size_t i = 0; i < n; ++i)
        std::printf("  #%-3u %-8s %04x:%04x  %s\n",
                    devs[i].id, clsName(devs[i].cls),
                    devs[i].vendorId, devs[i].productId, devs[i].name);
}

int main(int argc, char** argv) {
    bool listOnly = false;
    int  seconds  = 8;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--list")) listOnly = true;
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::atoi(argv[++i]);
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    setvbuf(stdout, nullptr, _IOLBF, 0);

    hid::Context ctx;
    if (!ctx.init({})) {
        std::fprintf(stderr, "hid_dump: init failed — %s\n", ctx.lastError());
        return 1;
    }
    // Give device-add callbacks a beat, then show what we see.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    printDevices(ctx);
    if (listOnly) return 0;

    std::printf("capturing %ds — move the mouse / press keys "
                "(Ctrl+C to stop early)…\n", seconds);

    hid::Event ev[512];
    uint64_t motionReports = 0, sumDx = 0, sumDy = 0;
    uint64_t totalEvents = 0, secMotion = 0;
    uint64_t peakRate = 0;
    uint64_t secStart = hid::nowNs();
    const uint64_t tEnd = hid::nowNs() + (uint64_t)seconds * 1000000000ull;

    while (!g_stop && hid::nowNs() < tEnd) {
        const size_t n = ctx.drain(ev, 512);
        for (size_t i = 0; i < n; ++i) {
            ++totalEvents;
            const hid::Event& e = ev[i];
            switch (e.type) {
            case hid::EventType::MouseMotion:
                ++motionReports; ++secMotion;
                sumDx += (uint64_t)std::abs(e.value);
                sumDy += (uint64_t)std::abs(e.value2);
                break;
            case hid::EventType::Key:
                std::printf("  [%.3f] dev#%u key %-6s %s\n",
                            e.timeNs / 1e9, e.device, keyName(e.code),
                            e.value ? "down" : "up");
                break;
            case hid::EventType::Button:
                std::printf("  [%.3f] dev#%u button %u %s\n",
                            e.timeNs / 1e9, e.device, e.code,
                            e.value ? "down" : "up");
                break;
            case hid::EventType::Scroll:
                std::printf("  [%.3f] dev#%u scroll h=%d v=%d\n",
                            e.timeNs / 1e9, e.device, e.value, e.value2);
                break;
            case hid::EventType::Axis:
                std::printf("  [%.3f] dev#%u axis 0x%02X = %d\n",
                            e.timeNs / 1e9, e.device, e.code, e.value);
                break;
            case hid::EventType::DeviceAdded:
                std::printf("  [%.3f] dev#%u CONNECTED (%s)\n",
                            e.timeNs / 1e9, e.device,
                            clsName((hid::DeviceClass)e.value));
                break;
            case hid::EventType::DeviceRemoved:
                std::printf("  [%.3f] dev#%u REMOVED (%s)\n",
                            e.timeNs / 1e9, e.device,
                            clsName((hid::DeviceClass)e.value));
                break;
            default: break;
            }
        }
        const uint64_t now = hid::nowNs();
        if (now - secStart >= 1000000000ull) {
            if (secMotion) {
                std::printf("  motion: %" PRIu64 " reports/s\n", secMotion);
                if (secMotion > peakRate) peakRate = secMotion;
            }
            secMotion = 0;
            secStart  = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::printf("── summary ──────────────────────────────────\n");
    std::printf("  events: %" PRIu64 "  motion reports: %" PRIu64
                "  |dx| %" PRIu64 "  |dy| %" PRIu64 " counts\n",
                totalEvents, motionReports, sumDx, sumDy);
    std::printf("  peak motion rate: %" PRIu64 " reports/s\n", peakRate);
    std::printf("  ring drops: %" PRIu64 "\n", ctx.dropped());
    std::printf("hid_dump: %s\n", totalEvents ? "PASS — events flowing"
                                              : "no events captured (idle?)");
    return 0;
}
