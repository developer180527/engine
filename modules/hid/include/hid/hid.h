#pragma once
// ── hid — thin cross-platform raw-input module ──────────────────────────────
//
// Devices and timestamped raw events. NOTHING else. This module reads the
// platform's low-level input stream (macOS IOHIDManager, Windows Raw Input,
// Linux evdev) BEFORE the OS pointer pipeline touches it: unaccelerated
// mouse counts, full report rate (1000–8000 Hz mice), per-device identity,
// hardware timestamps. It has zero engine dependencies — like assetlib, it
// is a standalone library any project can consume.
//
// WHAT DOES NOT BELONG HERE (by design — the consumer's policy layer owns
// these): action mapping, input contexts, UI capture/focus, dead zones,
// sensitivity, key display names, window management, text input/IME (that
// stays on the windowing layer, e.g. GLFW).
//
// THE CONTRACT
//   * One backend thread parks in the OS wait primitive and pushes POD
//     events into a lock-free SPSC ring. The consumer drains with pure
//     userspace reads — the game loop never makes an input syscall.
//   * Event::timeNs is ONE monotonic clock (hid::nowNs() reads the same
//     clock), normalized per-backend from the hardware/driver timestamp.
//     This is the seed for sub-tick input and netcode snapshots.
//   * The code space is HID usages — the native vocabulary of all three
//     backends: keyboard events carry Keyboard/Keypad page (0x07) usages,
//     mouse buttons carry a 0-based button index, gamepad axes carry
//     GenericDesktop usages. Translation to engine keycodes/actions is the
//     consumer's job.
//   * Ring overflow drops the NEWEST event and counts it (dropped()) — the
//     input thread never blocks.
//
// Schema stability: this header is the module's ABI. Add enum values and
// trailing Config fields freely; never reorder Event fields.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace hid {

using DeviceId = uint32_t;               // stable while connected; 0 = none

enum class DeviceClass : uint8_t {
    Unknown = 0,
    Mouse,          // includes trackpads in mouse-emulation
    Keyboard,
    Gamepad,        // gamepad/joystick — raw, uncalibrated (see info.md)
};

enum class EventType : uint8_t {
    None = 0,
    MouseMotion,    // value = dx counts, value2 = dy counts (either may be 0;
                    // backends may deliver axes as separate events — SUM both
                    // fields, never sample)
    Scroll,         // value = horizontal, value2 = vertical (detents/counts)
    Button,         // code = 0-based button index, value = 1 press / 0 release
    Key,            // code = HID usage (page 0x07), value = 1 press / 0 release
    Axis,           // code = GenericDesktop usage, value = raw logical value
    DeviceAdded,    // value = (int)DeviceClass
    DeviceRemoved,  // value = (int)DeviceClass
};

// 24-byte POD. timeNs first for cache-friendly time scans.
struct Event {
    uint64_t   timeNs;   // monotonic ns, same clock as hid::nowNs()
    DeviceId   device;
    EventType  type;
    uint8_t    _pad;
    uint16_t   code;
    int32_t    value;
    int32_t    value2;
};
static_assert(sizeof(Event) == 24, "Event is wire-format POD");

struct DeviceInfo {
    DeviceId    id;
    DeviceClass cls;
    uint16_t    vendorId;
    uint16_t    productId;
    char        name[64];   // UTF-8, truncated, always NUL-terminated
};

struct Config {
    bool mouse    = true;
    bool keyboard = true;
    bool gamepad  = true;
};

// Monotonic nanoseconds on the SAME clock as Event::timeNs.
uint64_t nowNs();

// ── EventRing — lock-free SPSC ring (one producer: the backend thread) ──────
// Public because consumers building their own staging (e.g. an engine input
// manager splitting per-tick) can reuse it. Capacity must be a power of two.
template <size_t N>
class EventRing {
    static_assert((N & (N - 1)) == 0, "capacity must be a power of two");
public:
    bool push(const Event& e) {                    // producer thread only
        const uint64_t h = m_head.load(std::memory_order_relaxed);
        const uint64_t t = m_tail.load(std::memory_order_acquire);
        if (h - t >= N) {
            m_drops.fetch_add(1, std::memory_order_relaxed);
            return false;                          // drop-newest, never block
        }
        m_buf[h & (N - 1)] = e;
        m_head.store(h + 1, std::memory_order_release);
        return true;
    }

    size_t drain(Event* out, size_t max) {         // consumer thread only
        const uint64_t t = m_tail.load(std::memory_order_relaxed);
        const uint64_t h = m_head.load(std::memory_order_acquire);
        size_t n = (size_t)(h - t);
        if (n > max) n = max;
        for (size_t i = 0; i < n; ++i) out[i] = m_buf[(t + i) & (N - 1)];
        m_tail.store(t + n, std::memory_order_release);
        return n;
    }

    uint64_t dropped() const { return m_drops.load(std::memory_order_relaxed); }

private:
    alignas(64) std::atomic<uint64_t> m_head{0};
    alignas(64) std::atomic<uint64_t> m_tail{0};
    alignas(64) std::atomic<uint64_t> m_drops{0};
    Event m_buf[N];
};

// ── Context — one per process is typical ────────────────────────────────────
// init() spawns the backend thread (or schedules onto one) and starts
// delivering; drain() from exactly ONE consumer thread. All methods are safe
// to call when init failed (they no-op / return 0).
class Context {
public:
    Context();
    ~Context();                                    // calls shutdown()
    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    // False on failure — lastError() explains (e.g. macOS Input Monitoring
    // permission not granted; see info.md).
    bool init(const Config& cfg = {});
    void shutdown();
    bool running() const;

    // Snapshot of currently connected matching devices.
    size_t devices(DeviceInfo* out, size_t max) const;

    // Consumer side of the ring. Returns events oldest-first.
    size_t drain(Event* out, size_t max);

    // Lifetime count of events lost to ring overflow (consumer too slow).
    uint64_t dropped() const;

    const char* lastError() const;

private:
    struct Impl;                                   // backend-owned
    Impl* m_impl = nullptr;
};

} // namespace hid
