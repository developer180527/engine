#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include "runtime/input_event.h"
#include "runtime/input_system.h"
#include "runtime/string_id.h"

// ── InputMap ───────────────────────────────────────────────────────────────
// Stores named action/axis bindings. Uses vectors (not hash maps) so the
// settings UI can iterate in insertion order and display names cleanly.
// Linear search over ≤20 entries is faster than hash lookup in practice
// (single cache line, no pointer chasing).

struct ActionDef {
    std::string      name; // display name
    StringID         id;   // fnv1a32(name) — cached for fast compare
    std::vector<Key> keys;
};

struct AxisDef {
    std::string name;
    StringID    id;
    Key         positive;
    Key         negative;
};

class InputMap {
public:
    static InputMap& get() { static InputMap inst; return inst; }

    // ── Registration ───────────────────────────────────────────────────────
    void bindAction(const std::string& name, Key key) {
        StringID id{name.c_str()};
        for (auto& a : m_actions) {
            if (a.id == id) { a.keys.push_back(key); return; }
        }
        m_actions.push_back({name, id, {key}});
    }

    void bindAxis(const std::string& name, Key positive, Key negative) {
        StringID id{name.c_str()};
        for (auto& a : m_axes) {
            if (a.id == id) { a.positive = positive; a.negative = negative; return; }
        }
        m_axes.push_back({name, id, positive, negative});
    }

    void clearAll() { m_actions.clear(); m_axes.clear(); }

    // ── Mutation (used by settings UI) ─────────────────────────────────────
    void addActionKey(StringID id, Key key) {
        for (auto& a : m_actions)
            if (a.id == id) { a.keys.push_back(key); return; }
    }

    void removeActionKey(StringID id, int keyIndex) {
        for (auto& a : m_actions) {
            if (a.id == id && keyIndex < (int)a.keys.size()) {
                a.keys.erase(a.keys.begin() + keyIndex);
                return;
            }
        }
    }

    void removeAction(StringID id) {
        m_actions.erase(std::remove_if(m_actions.begin(), m_actions.end(),
            [id](const ActionDef& a){ return a.id == id; }), m_actions.end());
    }

    void removeAxis(StringID id) {
        m_axes.erase(std::remove_if(m_axes.begin(), m_axes.end(),
            [id](const AxisDef& a){ return a.id == id; }), m_axes.end());
    }

    void setAxisPositive(StringID id, Key key) {
        for (auto& a : m_axes) if (a.id == id) { a.positive = key; return; }
    }

    void setAxisNegative(StringID id, Key key) {
        for (auto& a : m_axes) if (a.id == id) { a.negative = key; return; }
    }

    void addAction(const std::string& name) {
        StringID id{name.c_str()};
        for (auto& a : m_actions) if (a.id == id) return; // already exists
        m_actions.push_back({name, id, {}});
    }

    void addAxis(const std::string& name, Key positive = Key::Unknown,
                                          Key negative = Key::Unknown) {
        StringID id{name.c_str()};
        for (auto& a : m_axes) if (a.id == id) return;
        m_axes.push_back({name, id, positive, negative});
    }

    // ── Read (settings UI + serialization) ────────────────────────────────
    const std::vector<ActionDef>& actions() const { return m_actions; }
          std::vector<ActionDef>& actions()        { return m_actions; }
    const std::vector<AxisDef>&   axes()    const { return m_axes; }
          std::vector<AxisDef>&   axes()           { return m_axes; }

    // ── Action queries ─────────────────────────────────────────────────────
    bool isActionDown(StringID id) const {
        for (const auto& a : m_actions)
            if (a.id == id)
                for (Key k : a.keys)
                    if (InputSystem::get().isKeyDown((int)k)) return true;
        return false;
    }
    bool isActionPressed(StringID id) const {
        for (const auto& a : m_actions)
            if (a.id == id)
                for (Key k : a.keys)
                    if (InputSystem::get().isKeyPressed((int)k)) return true;
        return false;
    }
    bool isActionReleased(StringID id) const {
        for (const auto& a : m_actions)
            if (a.id == id)
                for (Key k : a.keys)
                    if (InputSystem::get().isKeyReleased((int)k)) return true;
        return false;
    }

    // ── Axis queries ───────────────────────────────────────────────────────
    float getAxis(StringID id) const {
        for (const auto& a : m_axes) {
            if (a.id != id) continue;
            float v = 0.0f;
            auto& sys = InputSystem::get();
            if (sys.isKeyDown((int)a.positive)) v += 1.0f;
            if (sys.isKeyDown((int)a.negative))  v -= 1.0f;
            return std::clamp(v, -1.0f, 1.0f);
        }
        return 0.0f;
    }

    // const char* overloads — implicit StringID
    bool  isActionDown    (const char* n) const { return isActionDown    (StringID{n}); }
    bool  isActionPressed (const char* n) const { return isActionPressed (StringID{n}); }
    bool  isActionReleased(const char* n) const { return isActionReleased(StringID{n}); }
    float getAxis         (const char* n) const { return getAxis         (StringID{n}); }

private:
    InputMap() = default;
    std::vector<ActionDef> m_actions;
    std::vector<AxisDef>   m_axes;
};
