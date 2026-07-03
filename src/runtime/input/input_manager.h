#pragma once
// ── InputManager — the engine's input policy layer ──────────────────────────
// Everything the hid module deliberately refuses to do, in one place:
//
//   ENDPOINT ELECTION   OSes split one physical device into several HID
//                       endpoints (collections). Endpoints share
//                       DeviceInfo.physId; the manager elects the FIRST
//                       endpoint that emits a given event type per physical
//                       group and drops that type from siblings — one click
//                       is one click, motion never double-counts.
//   SNAPSHOTS           beginTick(tickEndNs) folds events with
//                       timeNs <= tickEndNs into a deterministic POD
//                       InputSnapshot (key/button bitsets persist, deltas
//                       are per-tick). Same event stream in => bit-identical
//                       snapshots out — this is the multiplayer contract
//                       (prediction stores it, the wire carries it, rollback
//                       replays it). Later events stay staged for the next
//                       tick. The engine currently ticks per-frame; when a
//                       fixed-timestep sim lands, this API slots in as-is.
//   LATE-LATCH LOOK     consumeLook() drains ALL accumulated raw mouse
//                       counts — including events newer than the last tick —
//                       for the camera at render time. Freshest possible
//                       aim; unsimulated and unsmoothed by design; does not
//                       perturb snapshot determinism (separate accumulator).
//   FOCUS GATE          raw HID is system-wide; when unfocused the manager
//                       drops presses/motion but still applies releases (no
//                       stuck keys on alt-tab).
//   ACTIONS + CONTEXTS  gameplay is input-agnostic: named actions
//                       (digital / axis1 / axis2) resolved through a context
//                       stack (top first; a blockLower context stops
//                       fall-through). Bindings come from the project's
//                       input.json — the developer wires devices to actions;
//                       code never sees a key.
//
// Threading: pump() / beginTick() / queries all happen on the sim thread.
// Sources hand over events that were produced on their own threads.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "runtime/input/input_sources.h"

namespace input {

struct InputSnapshot {
    uint64_t tickEndNs   = 0;
    uint64_t keys[4]     = {};   // HID-usage bitset (0..255) — persists
    // Per-tick TRANSITION masks: a press+release landing inside ONE tick
    // leaves the held bitset unchanged (cur == prev) but must still fire
    // edge-triggered actions — high-frequency taps are real at 8kHz input
    // and 60Hz ticks. Reset every tick.
    uint64_t keysPressed[4]  = {};
    uint64_t keysReleased[4] = {};
    uint32_t mouseButtons = 0;   // bit N = button N held — persists
    uint32_t buttonsPressed = 0, buttonsReleased = 0;   // per-tick
    int32_t  mouseDx = 0, mouseDy = 0;   // raw counts within this tick
    int32_t  scrollX = 0, scrollY = 0;
    uint32_t _pad = 0;   // explicit: snapshots are memcmp'd/hashed/wired —
                         // NO indeterminate padding bytes allowed

    bool keyDown(uint16_t usage) const {
        return usage < 256 && (keys[usage >> 6] >> (usage & 63)) & 1;
    }
    bool keyPressed(uint16_t u) const {
        return u < 256 && (keysPressed[u >> 6] >> (u & 63)) & 1;
    }
    bool keyReleased(uint16_t u) const {
        return u < 256 && (keysReleased[u >> 6] >> (u & 63)) & 1;
    }
    bool buttonDown(uint16_t b) const {
        return b < 32 && (mouseButtons >> b) & 1;
    }
};

static_assert(sizeof(InputSnapshot) == 136,
              "InputSnapshot must stay padding-free POD (memcmp/hash/wire)");

enum class ActionType : uint8_t { Digital, Axis1, Axis2 };

class InputManager {
public:
    // Picks HidSource (raw) when the platform backend comes up, else
    // WindowSource. Loads projectRoot/input.json (scaffolds a default).
    void init(const std::filesystem::path& projectRoot);
    // Tests / replay: explicit source, bindings via loadConfigText().
    void initWithSource(std::unique_ptr<IInputSource> src);
    void shutdown();
    const char* sourceName() const { return m_source ? m_source->name() : "none"; }

    // ── Frame flow ─────────────────────────────────────────────────────────
    void pump();                        // poll source -> staging (per frame)
    void beginTick(uint64_t tickEndNs); // staging(<=tickEnd) -> snapshot
    const InputSnapshot& snapshot() const { return m_cur; }
    const InputSnapshot& prevSnapshot() const { return m_prev; }

    // Late-latch look, cumulative model: totals only ever grow; consumers
    // diff against their own last-read value. consumeLook keeps the classic
    // single-consumer drain semantics via an internal cursor; multi-consumer
    // code (editor cameras, debug tools) reads lookTotal and diffs itself —
    // no consumer can starve another (API review fix).
    void consumeLook(float* dx, float* dy);
    void lookTotal(double* x, double* y) const;

    void setFocused(bool f) { m_focused = f; }
    void setUICapture(bool keyboard, bool mouse) {
        m_uiKb = keyboard; m_uiMouse = mouse;
    }

    // ── Actions ────────────────────────────────────────────────────────────
    bool  actionDown(const char* name) const;
    bool  actionPressed(const char* name) const;
    bool  actionReleased(const char* name) const;
    float axis1(const char* name) const;
    void  axis2(const char* name, float* x, float* y) const;
    void  pushContext(const char* name);
    void  popContext();

    // Bindings from JSON text (file loader + tests share this).
    bool loadConfigText(const std::string& jsonText);
    // Load (or scaffold) projectRoot/input.json. init() calls this when a
    // project is known at boot; hosts that open a project LATER (the editor
    // boots projectless) must call it again on project open.
    void loadProjectBindings(const std::filesystem::path& projectRoot);

private:
    struct Binding {
        enum Kind : uint8_t { KeyUsage, MouseButton, MouseMotion, Scroll } kind;
        uint16_t code  = 0;      // usage / button index
        float    scale = 1.0f;   // sign carries digital axis direction
        uint8_t  comp  = 0;      // axis2 component this binding feeds (0=x,1=y)
    };
    struct Action {
        std::string          name;
        ActionType           type;
        std::vector<Binding> binds;
    };
    struct Context {
        std::string         name;
        std::vector<Action> actions;
        bool                blockLower = false;
    };

    const Action* resolve(const char* name) const;
    float evalAxis(const Action& a, const InputSnapshot& s, int comp) const;
    bool  evalDigital(const Action& a, const InputSnapshot& s) const;
    bool  parseBinding(const std::string& spec, ActionType type, Binding* out);

    // Election (see header comment).
    void refreshEndpoints();
    bool accept(const hid::Event& e);

    std::unique_ptr<IInputSource> m_source;    // raw (hid), may be null
    std::unique_ptr<IInputSource> m_window;    // window fallback, always on
    uint64_t m_lastRawMotionNs = 0;            // hybrid routing (see .cpp)
    bool     m_rawMotionLive   = false;        // for handoff logging

    struct Endpoint { uint32_t physId; hid::DeviceClass cls; };
    std::unordered_map<uint32_t, Endpoint> m_endpoints;          // DeviceId ->
    std::unordered_map<uint64_t, uint32_t> m_elected;            // physId|type ->

    std::vector<hid::Event> m_staging;   // pumped, not yet ticked
    InputSnapshot m_cur, m_prev;

    double m_lookTotalX = 0.0, m_lookTotalY = 0.0;   // cumulative, never reset
    double m_lookCursorX = 0.0, m_lookCursorY = 0.0;  // consumeLook's position

    std::vector<Context> m_contexts;
    std::vector<size_t>  m_stack;        // indices into m_contexts, top=back

    bool m_focused = true, m_uiKb = false, m_uiMouse = false;
};

} // namespace input
