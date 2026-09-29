#pragma once
// ── Input Bindings model — editing input.json, without a GUI ─────────────────
// The project's input.json as an editable document: contexts -> actions ->
// binding specs ("key:W"), with add/remove, Save & Apply (write, then hot-
// reload the InputManager — mid-play too) and Revert. Capturing a binding is
// two halves: the model remembers WHICH binding is waiting, and the front end
// hands it the key it saw (ImGui reads the input system; libgui reads its UI
// key events). The ImGui panel and the libgui front end drive this document.
#include "core/logger.h"
#include "runtime/input/hid_keymap.h"
#include "runtime/input/input_event.h"
#include "runtime/input/input_manager.h"

#include <json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace bindings {

// Which binding a capture is aimed at.
struct Slot {
    size_t context = 0, action = 0, binding = 0;
    bool operator==(const Slot&) const = default;
};

class Document {
public:
    // (Re)read <projectRoot>/input.json. False when there is none, or it has
    // no "contexts".
    bool load(const std::filesystem::path& projectRoot) {
        m_root = projectRoot;
        std::ifstream f(projectRoot / "input.json");
        std::stringstream ss; ss << f.rdbuf();
        m_json = nlohmann::json::parse(ss.str(), nullptr, false);
        m_loaded = !m_json.is_discarded() && m_json.contains("contexts") && m_json["contexts"].is_array();
        m_dirty = false;
        m_capturing = false;
        return m_loaded;
    }
    bool loaded() const { return m_loaded; }
    bool dirty() const { return m_dirty; }

    // Write, then apply to the running InputManager.
    void saveAndApply(input::InputManager& mgr) {
        std::ofstream out(m_root / "input.json");
        out << m_json.dump(2);
        out.close();
        mgr.loadProjectBindings(m_root);
        m_dirty = false;
        LOG_SUCCESS("Input", "bindings saved + applied");
    }
    void revert() { load(m_root); }

    // ── Reading the document ────────────────────────────────────────────────
    size_t      contextCount() const { return m_loaded ? m_json["contexts"].size() : 0; }
    std::string contextName(size_t c) const { return ctx(c).value("name", "unnamed"); }
    size_t      actionCount(size_t c) const { return ctx(c).contains("actions") ? ctx(c)["actions"].size() : 0; }
    std::string actionName(size_t c, size_t a) const { return act(c, a).value("name", "?"); }
    std::string actionType(size_t c, size_t a) const { return act(c, a).value("type", "digital"); }
    size_t      bindingCount(size_t c, size_t a) const {
        return act(c, a).contains("bindings") ? act(c, a)["bindings"].size() : 0;
    }
    std::string binding(const Slot& s) const {
        const auto& b = act(s.context, s.action)["bindings"][s.binding];
        return b.is_string() ? b.get<std::string>() : std::string();
    }

    // ── Editing it ──────────────────────────────────────────────────────────
    void setBinding(const Slot& s, const std::string& spec) {
        m_json["contexts"][s.context]["actions"][s.action]["bindings"][s.binding] = spec;
        m_dirty = true;
    }
    void addBinding(size_t c, size_t a, const std::string& spec = "key:F1") {
        m_json["contexts"][c]["actions"][a]["bindings"].push_back(spec);
        m_dirty = true;
    }
    void removeBinding(const Slot& s) {
        auto& b = m_json["contexts"][s.context]["actions"][s.action]["bindings"];
        if (s.binding < b.size()) { b.erase(b.begin() + (long)s.binding); m_dirty = true; }
        if (m_capturing && m_capture == s) m_capturing = false;
    }

    // ── Capture ─────────────────────────────────────────────────────────────
    void beginCapture(const Slot& s) { m_capture = s; m_capturing = true; }
    void cancelCapture() { m_capturing = false; }
    bool capturing(const Slot& s) const { return m_capturing && m_capture == s; }
    bool capturingAny() const { return m_capturing; }
    // The key the user pressed: becomes "key:<name>". False (still waiting)
    // for a key with no binding-spec name.
    bool acceptCapturedKey(Key k) {
        if (!m_capturing) return false;
        const char* n = input::nameFromGlfw((int)k);   // Key values are GLFW's
        if (!n) return false;
        setBinding(m_capture, std::string("key:") + n);
        m_capturing = false;
        return true;
    }

private:
    const nlohmann::json& ctx(size_t c) const { return m_json["contexts"][c]; }
    const nlohmann::json& act(size_t c, size_t a) const { return m_json["contexts"][c]["actions"][a]; }

    std::filesystem::path m_root;
    nlohmann::json        m_json;
    bool                  m_loaded = false, m_dirty = false, m_capturing = false;
    Slot                  m_capture;
};

}  // namespace bindings
