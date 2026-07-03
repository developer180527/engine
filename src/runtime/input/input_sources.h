#pragma once
// ── InputSource — where the InputManager's events come from ─────────────────
// The manager consumes SOURCES, never platform APIs. Three implementations:
//
//   HidSource     modules/hid raw path (unaccelerated, timestamped, full
//                 report rate). The real thing — used whenever the platform
//                 backend initializes (macOS IOHIDManager today; needs the
//                 Input Monitoring grant).
//   WindowSource  engine-side fallback synthesizing hid::Events from the
//                 GLFW-polled InputSystem — for platforms without a native
//                 backend yet, and for hosts denied Input Monitoring. Cooked
//                 (OS-accelerated) but functional. Lives HERE, not in the
//                 hid module, by project rule.
//   ReplaySource  scripted event streams — determinism tests now, netcode
//                 input replay later.
//
// All sources speak hid::Event so everything above (election, snapshots,
// actions) is identical regardless of origin.

#include <cstring>
#include <memory>
#include <vector>

#include <hid/hid.h>

#include "runtime/input/hid_keymap.h"
#include "runtime/input/input_system.h"

namespace input {

class IInputSource {
public:
    virtual ~IInputSource() = default;
    virtual const char* name() const = 0;
    virtual size_t poll(hid::Event* out, size_t max) = 0;
    virtual size_t devices(hid::DeviceInfo* out, size_t max) const = 0;
};

// ── HidSource ───────────────────────────────────────────────────────────────
class HidSource final : public IInputSource {
public:
    bool init() { return m_ctx.init({}); }
    const char* lastError() const { return m_ctx.lastError(); }

    const char* name() const override { return "hid(raw)"; }
    size_t poll(hid::Event* out, size_t max) override {
        return m_ctx.drain(out, max);
    }
    size_t devices(hid::DeviceInfo* out, size_t max) const override {
        return m_ctx.devices(out, max);
    }

private:
    hid::Context m_ctx;
};

// ── WindowSource ────────────────────────────────────────────────────────────
// Diffs InputSystem's per-frame double-buffered state into events. Call
// poll() AFTER InputSystem::processEvents() each frame. Two synthetic
// endpoints: mouse (device 1) and keyboard (device 2).
class WindowSource final : public IInputSource {
public:
    const char* name() const override { return "window(glfw fallback)"; }

    size_t poll(hid::Event* out, size_t max) override {
        auto& in = InputSystem::get();
        const uint64_t t = hid::nowNs();
        size_t n = 0;
        auto push = [&](hid::Event e) { if (n < max) out[n++] = e; };

        const float dx = in.mouseDeltaX(), dy = in.mouseDeltaY();
        if (dx != 0.0f || dy != 0.0f)
            push({t, 1, hid::EventType::MouseMotion, 0, 0,
                  (int32_t)dx, (int32_t)dy});
        const float sx = in.scrollDeltaX(), sy = in.scrollDeltaY();
        if (sx != 0.0f || sy != 0.0f)
            push({t, 1, hid::EventType::Scroll, 0, 0,
                  (int32_t)sx, (int32_t)sy});
        for (int b = 0; b < 8; ++b) {
            if (in.isMousePressed(b))
                push({t, 1, hid::EventType::Button, 0, (uint16_t)b, 1, 0});
            else if (in.isMouseReleased(b))
                push({t, 1, hid::EventType::Button, 0, (uint16_t)b, 0, 0});
        }
        for (int k = 32; k <= 348; ++k) {
            const uint16_t usage = usageFromGlfw(k);
            if (!usage) continue;
            if (in.isKeyPressed(k))
                push({t, 2, hid::EventType::Key, 0, usage, 1, 0});
            else if (in.isKeyReleased(k))
                push({t, 2, hid::EventType::Key, 0, usage, 0, 0});
        }
        return n;
    }

    size_t devices(hid::DeviceInfo* out, size_t max) const override {
        static const hid::DeviceInfo kDevs[2] = {
            {1, hid::DeviceClass::Mouse,    0, 0, 1, "window mouse"},
            {2, hid::DeviceClass::Keyboard, 0, 0, 2, "window keyboard"},
        };
        const size_t n = max < 2 ? max : 2;
        for (size_t i = 0; i < n; ++i) out[i] = kDevs[i];
        return n;
    }
};

// ── ReplaySource ────────────────────────────────────────────────────────────
class ReplaySource final : public IInputSource {
public:
    void addDevice(const hid::DeviceInfo& d) { m_devs.push_back(d); }
    void addEvent(const hid::Event& e) { m_events.push_back(e); }

    const char* name() const override { return "replay"; }
    size_t poll(hid::Event* out, size_t max) override {
        size_t n = m_events.size() - m_cursor;
        if (n > max) n = max;
        for (size_t i = 0; i < n; ++i) out[i] = m_events[m_cursor + i];
        m_cursor += n;
        return n;
    }
    size_t devices(hid::DeviceInfo* out, size_t max) const override {
        size_t n = m_devs.size() < max ? m_devs.size() : max;
        for (size_t i = 0; i < n; ++i) out[i] = m_devs[i];
        return n;
    }
    void rewind() { m_cursor = 0; }

private:
    std::vector<hid::DeviceInfo> m_devs;
    std::vector<hid::Event>      m_events;
    size_t                       m_cursor = 0;
};

} // namespace input
