#include "runtime/input/input_manager.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <json.hpp>

#include "core/logger.h"
#include "runtime/input/hid_keymap.h"

namespace input {
namespace {

// The scaffold written to projects without an input.json — FPS defaults the
// developer edits freely (this file is theirs, never overwritten).
constexpr const char* kDefaultConfig = R"({
  "contexts": [
    {
      "name": "Gameplay",
      "actions": [
        { "name": "Fire",     "type": "digital", "bindings": ["mouse:left"] },
        { "name": "Aim",      "type": "digital", "bindings": ["mouse:right"] },
        { "name": "Jump",     "type": "digital", "bindings": ["key:Space"] },
        { "name": "Sprint",   "type": "digital", "bindings": ["key:LShift"] },
        { "name": "Interact", "type": "digital", "bindings": ["key:F"] },
        { "name": "ToggleCursor", "type": "digital", "bindings": ["key:Escape"] },
        { "name": "Move",     "type": "axis2",
          "bindings": ["key:W:+y", "key:S:-y", "key:D:+x", "key:A:-x"] },
        { "name": "Look",     "type": "axis2", "bindings": ["mouse:motion"] },
        { "name": "Zoom",     "type": "axis1", "bindings": ["scroll"] }
      ]
    }
  ]
})";

// Window-motion suppression window after raw motion. The GLFW echo of a raw
// mouse arrives within a frame or two, so a short window kills double-counts;
// a long one reads as trackpad lag on the raw->window handoff (user-felt at
// 1s, imperceptible at 250ms).
constexpr uint64_t kRawHandoffNs = 250000000ull;   // 250 ms

uint64_t electKey(uint32_t physId, hid::EventType t) {
    return ((uint64_t)physId << 8) | (uint64_t)t;
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────
void InputManager::init(const std::filesystem::path& projectRoot) {
    // HYBRID SOURCING. Raw HID covers what it is genuinely better at: mouse
    // motion/scroll from devices that emit relative counts (gaming mice).
    // Internal trackpads are DIGITIZERS — the OS synthesizes the pointer
    // later, so they emit no raw motion at all — and keyboards gain nothing
    // from raw (typing wants focus semantics). So: keyboard + buttons come
    // from the window path always; motion/scroll come raw while a raw mouse
    // is talking, window otherwise (trackpad keeps working, gaming mouse
    // never double-counts).
    m_window = std::make_unique<WindowSource>();
    auto raw = std::make_unique<HidSource>();
    if (raw->init()) {
        m_source = std::move(raw);
        LOG_SUCCESS("Input", "source: %s + window hybrid (raw motion, "
                    "window keys/buttons)", m_source->name());
    } else {
        LOG_WARN("Input", "raw backend unavailable (%s) — window input only",
                 raw->lastError());
    }
    refreshEndpoints();

    loadProjectBindings(projectRoot);
}

// Project bindings: load or scaffold. The file belongs to the developer.
void InputManager::loadProjectBindings(const std::filesystem::path& projectRoot) {
    if (projectRoot.empty()) return;   // projectless boot: editor hub
    const auto path = projectRoot / "input.json";
    std::ifstream f(path);
    if (f) {
        std::stringstream ss; ss << f.rdbuf();
        if (!loadConfigText(ss.str()))
            LOG_ERROR("Input", "input.json invalid — no bindings loaded");
    } else {
        std::ofstream out(path);
        out << kDefaultConfig;
        LOG_INFO("Input", "scaffolded default input.json");
        loadConfigText(kDefaultConfig);
    }
}

void InputManager::initWithSource(std::unique_ptr<IInputSource> src) {
    m_source = std::move(src);
    refreshEndpoints();
}

bool InputManager::startRecording(const std::filesystem::path& path) {
    stopRecording();
    auto f = std::make_unique<std::ofstream>(path, std::ios::binary);
    if (!f->good()) {
        LOG_ERROR("Input", "recording failed to open: %s", path.string().c_str());
        return false;
    }
    const uint32_t magic = 0x43455249, version = 1;   // 'IREC'
    f->write(reinterpret_cast<const char*>(&magic), 4);
    f->write(reinterpret_cast<const char*>(&version), 4);
    m_record = std::move(f);
    LOG_SUCCESS("Input", "recording input -> %s", path.string().c_str());
    return true;
}

void InputManager::stopRecording() {
    if (m_record) LOG_INFO("Input", "input recording closed");
    m_record.reset();
}

void InputManager::shutdown() {
    stopRecording();
    m_source.reset();
    m_window.reset();
    m_lastRawMotionNs = 0;
    m_endpoints.clear(); m_elected.clear();
    m_staging.clear(); m_contexts.clear(); m_stack.clear();
    m_cur = m_prev = {};
    m_lookTotalX = m_lookTotalY = 0.0;
    m_lookCursorX = m_lookCursorY = 0.0;
}

// ── Election ────────────────────────────────────────────────────────────────
void InputManager::refreshEndpoints() {
    m_endpoints.clear();
    hid::DeviceInfo devs[64];
    for (IInputSource* src : {m_source.get(), m_window.get()}) {
        if (!src) continue;
        const size_t n = src->devices(devs, 64);
        for (size_t i = 0; i < n; ++i)
            m_endpoints[devs[i].id] = {devs[i].physId, devs[i].cls};
    }
}

bool InputManager::accept(const hid::Event& e) {
    // Hotplug maintains the endpoint table; the events pass through too so
    // consumers watching connects still see them staged.
    if (e.type == hid::EventType::DeviceAdded) { refreshEndpoints(); return true; }
    if (e.type == hid::EventType::DeviceRemoved) {
        refreshEndpoints();
        for (auto it = m_elected.begin(); it != m_elected.end();)
            it = (it->second == e.device) ? m_elected.erase(it) : ++it;
        return true;
    }

    auto it = m_endpoints.find(e.device);
    const uint32_t phys = it != m_endpoints.end() ? it->second.physId : e.device;
    auto [el, inserted] = m_elected.try_emplace(electKey(phys, e.type), e.device);
    if (el->second != e.device) return false;   // a sibling endpoint owns this type

    // Focus/UI gates: releases always pass (no stuck keys); presses and
    // motion only when the game should be receiving input.
    const bool press = (e.type == hid::EventType::Key ||
                        e.type == hid::EventType::Button) && e.value != 0;
    const bool motion = e.type == hid::EventType::MouseMotion ||
                        e.type == hid::EventType::Scroll ||
                        e.type == hid::EventType::Axis;
    if (!m_focused && (press || motion)) return false;
    if (m_uiKb && e.type == hid::EventType::Key && press) return false;
    if (m_uiMouse && (motion || (press && e.type == hid::EventType::Button)))
        return false;
    return true;
}

// ── Frame flow ──────────────────────────────────────────────────────────────
void InputManager::pump() {
    hid::Event ev[512];
    auto drain = [&](IInputSource* src, bool isRaw) {
        if (!src) return;
        for (;;) {
            const size_t n = src->poll(ev, 512);
            for (size_t i = 0; i < n; ++i) {
                const hid::Event& e = ev[i];
                const bool motionKind = e.type == hid::EventType::MouseMotion ||
                                        e.type == hid::EventType::Scroll;
                if (isRaw) {
                    // Raw path carries ONLY motion/scroll (+ device events).
                    if (e.type == hid::EventType::Key ||
                        e.type == hid::EventType::Button) continue;
                    if (motionKind) m_lastRawMotionNs = e.timeNs;
                    if (motionKind && !m_rawMotionLive) {
                        m_rawMotionLive = true;
                        LOG_INFO("Input", "motion source -> RAW (gaming mouse talking)");
                    }
                } else if (motionKind && m_source &&
                           hid::nowNs() - m_lastRawMotionNs < kRawHandoffNs) {
                    continue;   // raw mouse active — window motion is its echo
                } else if (motionKind && m_rawMotionLive) {
                    m_rawMotionLive = false;
                    LOG_INFO("Input", "motion source -> WINDOW (trackpad/idle handoff)");
                }
                if (!accept(e)) continue;
                m_staging.push_back(e);
                if (e.type == hid::EventType::MouseMotion) {
                    m_lookTotalX += e.value;    // cumulative — never reset;
                    m_lookTotalY += e.value2;   // consumers diff (see header)
                    m_newestMotionNs = e.timeNs;
                }
                { // latency: how long the event sat between device and pump
                    const uint64_t now = hid::nowNs();
                    const uint64_t q = now > e.timeNs ? now - e.timeNs : 0;
                    ++m_lat.events;
                    m_lat.queueSumNs += q;
                    if (q > m_lat.queueMaxNs) m_lat.queueMaxNs = q;
                }
                if (m_record)
                    m_record->write(reinterpret_cast<const char*>(&e), sizeof(e));
            }
            if (n < 512) break;
        }
    };
    drain(m_source.get(), m_source && m_source->rawMotionOnly());
    drain(m_window.get(), false);
}

void InputManager::beginTick(uint64_t tickEndNs) {
    const uint64_t tickStartNs = m_cur.tickEndNs;   // previous boundary
    if (m_record) {   // tick boundary marker (type None, code 0xTCK)
        hid::Event mark{tickEndNs, 0, hid::EventType::None, 0, 0x7C4, 0, 0};
        m_record->write(reinterpret_cast<const char*>(&mark), sizeof(mark));
        m_record->flush();   // dev tool: survive kills/crashes losslessly
    }
    uint64_t oldest = UINT64_MAX;
    for (const hid::Event& e : m_staging)
        if (e.timeNs <= tickEndNs && e.timeNs < oldest) oldest = e.timeNs;
    if (oldest != UINT64_MAX && tickEndNs > oldest)
        m_lat.tickLagNs = tickEndNs - oldest;
    m_prev = m_cur;
    m_cur.tickEndNs = tickEndNs;
    m_cur.mouseDx = m_cur.mouseDy = m_cur.scrollX = m_cur.scrollY = 0;
    std::memset(m_cur.keysPressed,  0, sizeof(m_cur.keysPressed));
    std::memset(m_cur.keysReleased, 0, sizeof(m_cur.keysReleased));
    m_cur.buttonsPressed = m_cur.buttonsReleased = 0;
    std::memset(m_cur.edges, 0, sizeof(m_cur.edges));
    m_cur.edgeCount = 0;
    auto pushEdge = [&](uint16_t code, uint8_t kind, uint8_t down,
                        uint64_t tNs) {
        if (m_cur.edgeCount >= InputSnapshot::kMaxEdges) return;
        const uint64_t off = tNs > tickStartNs ? tNs - tickStartNs : 0;
        m_cur.edges[m_cur.edgeCount++] =
            { code, kind, down, (uint32_t)(off / 1000ull) };
    };

    size_t kept = 0;
    for (size_t i = 0; i < m_staging.size(); ++i) {
        const hid::Event& e = m_staging[i];
        if (e.timeNs > tickEndNs) { m_staging[kept++] = e; continue; }
        switch (e.type) {
        case hid::EventType::Key:
            if (e.code < 256) {
                const uint64_t bit = 1ull << (e.code & 63);
                if (e.value) { m_cur.keys[e.code >> 6] |= bit;
                               m_cur.keysPressed[e.code >> 6]  |= bit; }
                else         { m_cur.keys[e.code >> 6] &= ~bit;
                               m_cur.keysReleased[e.code >> 6] |= bit; }
                pushEdge(e.code, 0, e.value ? 1 : 0, e.timeNs);
            }
            break;
        case hid::EventType::Button:
            if (e.code < 32) {
                const uint32_t bit = 1u << e.code;
                if (e.value) { m_cur.mouseButtons |= bit;
                               m_cur.buttonsPressed  |= bit; }
                else         { m_cur.mouseButtons &= ~bit;
                               m_cur.buttonsReleased |= bit; }
                pushEdge(e.code, 1, e.value ? 1 : 0, e.timeNs);
            }
            break;
        case hid::EventType::MouseMotion:
            m_cur.mouseDx += e.value; m_cur.mouseDy += e.value2;
            break;
        case hid::EventType::Scroll:
            m_cur.scrollX += e.value; m_cur.scrollY += e.value2;
            break;
        default: break;
        }
    }
    m_staging.resize(kept);
}

void InputManager::consumeLook(float* dx, float* dy) {
    if (m_newestMotionNs) {
        const uint64_t now = hid::nowNs();
        if (now > m_newestMotionNs)
            m_lat.lookLagNs = now - m_newestMotionNs;
    }
    if (dx) *dx = (float)(m_lookTotalX - m_lookCursorX);
    if (dy) *dy = (float)(m_lookTotalY - m_lookCursorY);
    m_lookCursorX = m_lookTotalX;
    m_lookCursorY = m_lookTotalY;
}

void InputManager::lookTotal(double* x, double* y) const {
    if (x) *x = m_lookTotalX;
    if (y) *y = m_lookTotalY;
}

// ── Actions ─────────────────────────────────────────────────────────────────
const InputManager::Action* InputManager::resolve(const char* name) const {
    for (size_t s = m_stack.size(); s-- > 0;) {
        const Context& c = m_contexts[m_stack[s]];
        for (const Action& a : c.actions)
            if (a.name == name) return &a;
        if (c.blockLower) return nullptr;
    }
    return nullptr;
}

bool InputManager::evalDigital(const Action& a, const InputSnapshot& s) const {
    for (const Binding& b : a.binds) {
        if (b.kind == Binding::KeyUsage    && s.keyDown(b.code))    return true;
        if (b.kind == Binding::MouseButton && s.buttonDown(b.code)) return true;
    }
    return false;
}

float InputManager::evalAxis(const Action& a, const InputSnapshot& s,
                             int comp) const {
    float v = 0.0f;
    for (const Binding& b : a.binds) {
        if (a.type == ActionType::Axis2 && b.comp != comp &&
            b.kind != Binding::MouseMotion && b.kind != Binding::Scroll)
            continue;   // motion/scroll are inherently 2D — query comp selects
        switch (b.kind) {
        case Binding::KeyUsage:
            if (s.keyDown(b.code)) v += b.scale;
            break;
        case Binding::MouseButton:
            if (s.buttonDown(b.code)) v += b.scale;
            break;
        case Binding::MouseMotion:
            v += b.scale * (comp == 0 ? (float)s.mouseDx : (float)s.mouseDy);
            break;
        case Binding::Scroll:
            v += b.scale * (comp == 0 ? (float)s.scrollX : (float)s.scrollY);
            break;
        }
    }
    return v;
}

uint32_t InputManager::actionPressedOffsetUs(const char* n) const {
    const Action* a = resolve(n);
    if (!a || a->type != ActionType::Digital) return UINT32_MAX;
    for (uint32_t i = 0; i < m_cur.edgeCount; ++i) {
        const auto& e = m_cur.edges[i];
        if (!e.down) continue;
        for (const Binding& b : a->binds) {
            if (b.kind == Binding::KeyUsage    && e.kind == 0 && b.code == e.code) return e.offsetUs;
            if (b.kind == Binding::MouseButton && e.kind == 1 && b.code == e.code) return e.offsetUs;
        }
    }
    return UINT32_MAX;
}

bool InputManager::actionDown(const char* n) const {
    const Action* a = resolve(n);
    return a && a->type == ActionType::Digital && evalDigital(*a, m_cur);
}
bool InputManager::actionPressed(const char* n) const {
    const Action* a = resolve(n);
    if (!a || a->type != ActionType::Digital) return false;
    if (evalDigital(*a, m_cur) && !evalDigital(*a, m_prev)) return true;
    for (const Binding& b : a->binds) {   // sub-tick tap: transition mask
        if (b.kind == Binding::KeyUsage    && m_cur.keyPressed(b.code)) return true;
        if (b.kind == Binding::MouseButton && (m_cur.buttonsPressed >> b.code) & 1) return true;
    }
    return false;
}
bool InputManager::actionReleased(const char* n) const {
    const Action* a = resolve(n);
    if (!a || a->type != ActionType::Digital) return false;
    if (!evalDigital(*a, m_cur) && evalDigital(*a, m_prev)) return true;
    for (const Binding& b : a->binds) {
        if (b.kind == Binding::KeyUsage    && m_cur.keyReleased(b.code)) return true;
        if (b.kind == Binding::MouseButton && (m_cur.buttonsReleased >> b.code) & 1) return true;
    }
    return false;
}
float InputManager::axis1(const char* n) const {
    const Action* a = resolve(n);
    // Axis1 bindings feed the y slot (scroll is vertical-first).
    return a && a->type == ActionType::Axis1 ? evalAxis(*a, m_cur, 1) : 0.0f;
}
void InputManager::axis2(const char* n, float* x, float* y) const {
    const Action* a = resolve(n);
    const bool ok = a && a->type == ActionType::Axis2;
    if (x) *x = ok ? evalAxis(*a, m_cur, 0) : 0.0f;
    if (y) *y = ok ? evalAxis(*a, m_cur, 1) : 0.0f;
}

void InputManager::pushContext(const char* name) {
    for (size_t i = 0; i < m_contexts.size(); ++i)
        if (m_contexts[i].name == name) { m_stack.push_back(i); return; }
    LOG_WARN("Input", "pushContext: no context named '%s'", name);
}
void InputManager::popContext() {
    if (m_stack.size() > 1) m_stack.pop_back();   // bottom context stays
}

// ── Binding specs ───────────────────────────────────────────────────────────
//   "key:W"          digital / axis contribution (axis: "key:W:+y")
//   "mouse:left|right|middle|N"       buttons
//   "mouse:motion[:scale]"            axis2 from raw counts
//   "scroll[:scale]"                  axis1 from wheel
bool InputManager::parseBinding(const std::string& spec, ActionType type,
                                Binding* out) {
    std::vector<std::string> parts;
    for (size_t p = 0; p < spec.size();) {
        size_t c = spec.find(':', p);
        if (c == std::string::npos) c = spec.size();
        parts.push_back(spec.substr(p, c - p));
        p = c + 1;
    }
    if (parts.empty()) return false;
    auto axisSuffix = [&](const std::string& s) {   // "+y" / "-x"
        if (s.size() == 2 && (s[0] == '+' || s[0] == '-')) {
            out->scale = s[0] == '-' ? -1.0f : 1.0f;
            out->comp  = s[1] == 'y' ? 1 : 0;
            return true;
        }
        return false;
    };

    if (parts[0] == "key" && parts.size() >= 2) {
        out->kind = Binding::KeyUsage;
        out->code = usageFromName(parts[1].c_str());
        if (!out->code) return false;
        if (type == ActionType::Axis2)
            return parts.size() >= 3 && axisSuffix(parts[2]);
        if (parts.size() >= 3) out->scale = std::stof(parts[2]);
        return true;
    }
    if (parts[0] == "mouse" && parts.size() >= 2) {
        if (parts[1] == "motion") {
            out->kind  = Binding::MouseMotion;
            out->scale = parts.size() >= 3 ? std::stof(parts[2]) : 1.0f;
            return true;
        }
        out->kind = Binding::MouseButton;
        if      (parts[1] == "left")   out->code = 0;
        else if (parts[1] == "right")  out->code = 1;
        else if (parts[1] == "middle") out->code = 2;
        else                           out->code = (uint16_t)std::stoi(parts[1]);
        if (type == ActionType::Axis2)
            return parts.size() >= 3 && axisSuffix(parts[2]);
        return true;
    }
    // TODO(#13 gamepad): "pad:button:N", "pad:axis:<usage>[:scale]" specs
    // land here; hid already delivers raw Axis/Button events for
    // DeviceClass::Gamepad — the curation layer (GCController/XInput -> a
    // stable pad model with deadzones per binding) is the missing piece.
    if (parts[0] == "pad") {
        LOG_WARN("Input", "'pad:' bindings not implemented yet (backlog #13)");
        return false;
    }
    if (parts[0] == "scroll") {
        out->kind  = Binding::Scroll;
        out->scale = parts.size() >= 2 ? std::stof(parts[1]) : 1.0f;
        return true;
    }
    return false;
}

bool InputManager::loadConfigText(const std::string& jsonText) {
    nlohmann::json j = nlohmann::json::parse(jsonText, nullptr, false);
    if (j.is_discarded() || !j.contains("contexts")) return false;

    m_contexts.clear(); m_stack.clear();
    for (const auto& jc : j["contexts"]) {
        Context c;
        c.name       = jc.value("name", "unnamed");
        c.blockLower = jc.value("blockLower", false);
        for (const auto& ja : jc.value("actions", nlohmann::json::array())) {
            Action a;
            a.name = ja.value("name", "");
            const std::string t = ja.value("type", "digital");
            a.type = t == "axis2" ? ActionType::Axis2
                   : t == "axis1" ? ActionType::Axis1 : ActionType::Digital;
            for (const auto& jb : ja.value("bindings", nlohmann::json::array())) {
                Binding b{};
                if (parseBinding(jb.get<std::string>(), a.type, &b))
                    a.binds.push_back(b);
                else
                    LOG_WARN("Input", "bad binding '%s' on action '%s'",
                             jb.get<std::string>().c_str(), a.name.c_str());
            }
            if (!a.name.empty()) c.actions.push_back(std::move(a));
        }
        m_contexts.push_back(std::move(c));
    }
    if (!m_contexts.empty()) m_stack.push_back(0);   // first context = base
    LOG_INFO("Input", "%zu context(s) loaded, base '%s'",
             m_contexts.size(),
             m_contexts.empty() ? "-" : m_contexts[0].name.c_str());
    return !m_contexts.empty();
}

} // namespace input
