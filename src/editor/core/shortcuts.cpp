#include "editor/core/shortcuts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace shortcuts {
namespace {

struct KeyEntry { Key key; const char* name; const char* apple; const char* pc; };

// One table for parsing, formatting and menu spelling. `apple`/`pc` are the
// menu glyphs where they differ from the name; nullptr means "use the name".
const KeyEntry kKeys[] = {
    {Key::Space, "Space", "Space", "Space"},
    {Key::Escape, "Escape", "⎋", "Esc"},
    {Key::Enter, "Enter", "↩", "Enter"},
    {Key::Tab, "Tab", "⇥", "Tab"},
    {Key::Backspace, "Backspace", "⌫", "Backspace"},
    {Key::Delete, "Delete", "⌦", "Del"},
    {Key::Insert, "Insert", nullptr, "Ins"},
    {Key::Right, "Right", "→", "Right"}, {Key::Left, "Left", "←", "Left"},
    {Key::Down, "Down", "↓", "Down"},    {Key::Up, "Up", "↑", "Up"},
    {Key::PageUp, "PageUp", "⇞", "PgUp"}, {Key::PageDown, "PageDown", "⇟", "PgDn"},
    {Key::Home, "Home", "↖", "Home"},     {Key::End, "End", "↘", "End"},
    {Key::CapsLock, "CapsLock", "⇪", "CapsLock"}, {Key::Menu, "Menu", nullptr, nullptr},
    {Key::Apostrophe, "Apostrophe", "'", "'"}, {Key::Comma, "Comma", ",", ","},
    {Key::Minus, "Minus", "-", "-"},          {Key::Period, "Period", ".", "."},
    {Key::Slash, "Slash", "/", "/"},          {Key::Semicolon, "Semicolon", ";", ";"},
    {Key::Equal, "Equal", "=", "="},          {Key::LeftBracket, "LeftBracket", "[", "["},
    {Key::Backslash, "Backslash", "\\", "\\"}, {Key::RightBracket, "RightBracket", "]", "]"},
    {Key::GraveAccent, "Grave", "`", "`"},
    {Key::A, "A", nullptr, nullptr}, {Key::B, "B", nullptr, nullptr}, {Key::C, "C", nullptr, nullptr},
    {Key::D, "D", nullptr, nullptr}, {Key::E, "E", nullptr, nullptr}, {Key::F, "F", nullptr, nullptr},
    {Key::G, "G", nullptr, nullptr}, {Key::H, "H", nullptr, nullptr}, {Key::I, "I", nullptr, nullptr},
    {Key::J, "J", nullptr, nullptr}, {Key::K, "K", nullptr, nullptr}, {Key::L, "L", nullptr, nullptr},
    {Key::M, "M", nullptr, nullptr}, {Key::N, "N", nullptr, nullptr}, {Key::O, "O", nullptr, nullptr},
    {Key::P, "P", nullptr, nullptr}, {Key::Q, "Q", nullptr, nullptr}, {Key::R, "R", nullptr, nullptr},
    {Key::S, "S", nullptr, nullptr}, {Key::T, "T", nullptr, nullptr}, {Key::U, "U", nullptr, nullptr},
    {Key::V, "V", nullptr, nullptr}, {Key::W, "W", nullptr, nullptr}, {Key::X, "X", nullptr, nullptr},
    {Key::Y, "Y", nullptr, nullptr}, {Key::Z, "Z", nullptr, nullptr},
    {Key::Num0, "0", nullptr, nullptr}, {Key::Num1, "1", nullptr, nullptr}, {Key::Num2, "2", nullptr, nullptr},
    {Key::Num3, "3", nullptr, nullptr}, {Key::Num4, "4", nullptr, nullptr}, {Key::Num5, "5", nullptr, nullptr},
    {Key::Num6, "6", nullptr, nullptr}, {Key::Num7, "7", nullptr, nullptr}, {Key::Num8, "8", nullptr, nullptr},
    {Key::Num9, "9", nullptr, nullptr},
    {Key::F1, "F1", nullptr, nullptr}, {Key::F2, "F2", nullptr, nullptr}, {Key::F3, "F3", nullptr, nullptr},
    {Key::F4, "F4", nullptr, nullptr}, {Key::F5, "F5", nullptr, nullptr}, {Key::F6, "F6", nullptr, nullptr},
    {Key::F7, "F7", nullptr, nullptr}, {Key::F8, "F8", nullptr, nullptr}, {Key::F9, "F9", nullptr, nullptr},
    {Key::F10, "F10", nullptr, nullptr}, {Key::F11, "F11", nullptr, nullptr}, {Key::F12, "F12", nullptr, nullptr},
    {Key::Kp0, "Kp0", nullptr, "Num0"}, {Key::Kp1, "Kp1", nullptr, "Num1"}, {Key::Kp2, "Kp2", nullptr, "Num2"},
    {Key::Kp3, "Kp3", nullptr, "Num3"}, {Key::Kp4, "Kp4", nullptr, "Num4"}, {Key::Kp5, "Kp5", nullptr, "Num5"},
    {Key::Kp6, "Kp6", nullptr, "Num6"}, {Key::Kp7, "Kp7", nullptr, "Num7"}, {Key::Kp8, "Kp8", nullptr, "Num8"},
    {Key::Kp9, "Kp9", nullptr, "Num9"},
    {Key::KpDecimal, "KpDecimal", nullptr, "Num."}, {Key::KpDivide, "KpDivide", nullptr, "Num/"},
    {Key::KpMultiply, "KpMultiply", nullptr, "Num*"}, {Key::KpSubtract, "KpSubtract", nullptr, "Num-"},
    {Key::KpAdd, "KpAdd", nullptr, "Num+"}, {Key::KpEnter, "KpEnter", "⌤", "NumEnter"},
    {Key::KpEqual, "KpEqual", nullptr, "Num="},
};

const KeyEntry* entryFor(Key k) {
    for (const auto& e : kKeys) if (e.key == k) return &e;
    return nullptr;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.remove_suffix(1);
    return s;
}

}  // namespace

uint8_t abstractMods(const uiin::Modifiers& m, KeyboardConvention c) {
    uint8_t r = ModNone;
    if (m.shift) r |= ModShift;
    if (m.alt)   r |= ModAlt;
    const bool primaryDown   = c == KeyboardConvention::Apple ? m.super : m.ctrl;
    const bool secondaryDown = c == KeyboardConvention::Apple ? m.ctrl  : m.super;
    if (primaryDown)   r |= ModPrimary;
    if (secondaryDown) r |= ModSecondary;
    return r;
}

const char* keyName(Key k) {
    const KeyEntry* e = entryFor(k);
    return e ? e->name : nullptr;
}

std::optional<Key> keyFromName(std::string_view name) {
    for (const auto& e : kKeys) if (iequals(name, e.name)) return e.key;
    return std::nullopt;
}

std::optional<Chord> parseChord(std::string_view text) {
    Chord c;
    text = trim(text);
    if (text.empty()) return std::nullopt;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find('+', start);
        // A trailing "+" names nothing; "Primary++" is not "Primary+Plus".
        std::string_view tok = trim(text.substr(start, plus == std::string_view::npos
                                                       ? std::string_view::npos : plus - start));
        const bool last = plus == std::string_view::npos;
        if (tok.empty()) return std::nullopt;
        if (!last) {
            uint8_t bit = 0;
            if      (iequals(tok, "Primary"))   bit = ModPrimary;
            else if (iequals(tok, "Secondary")) bit = ModSecondary;
            else if (iequals(tok, "Shift"))     bit = ModShift;
            else if (iequals(tok, "Alt"))       bit = ModAlt;
            else return std::nullopt;           // physical names (Ctrl, Cmd) are ambiguous here
            if (c.mods & bit) return std::nullopt;   // "Shift+Shift+S"
            c.mods |= bit;
            start = plus + 1;
        } else {
            auto k = keyFromName(tok);
            if (!k) return std::nullopt;
            c.key = *k;
            return c;
        }
    }
    return std::nullopt;
}

std::string formatChord(const Chord& c) {
    std::string s;
    if (c.mods & ModPrimary)   s += "Primary+";
    if (c.mods & ModSecondary) s += "Secondary+";
    if (c.mods & ModAlt)       s += "Alt+";
    if (c.mods & ModShift)     s += "Shift+";
    const char* n = keyName(c.key);
    s += n ? n : "?";
    return s;
}

bool ShortcutMap::define(CommandSpec spec) {
    if (spec.id.empty() || find(spec.id)) return false;
    m_commands.push_back(std::move(spec));
    return true;
}

const CommandSpec* ShortcutMap::find(std::string_view id) const {
    for (const auto& c : m_commands) if (c.id == id) return &c;
    return nullptr;
}

const ShortcutMap::Override* ShortcutMap::findOverride(std::string_view id) const {
    for (const auto& o : m_overrides) if (o.id == id) return &o;
    return nullptr;
}

std::vector<Chord> ShortcutMap::chords(std::string_view id) const {
    if (const Override* o = findOverride(id)) return o->chords;
    std::vector<Chord> out;
    if (const CommandSpec* c = find(id))
        for (const auto& b : c->defaults)
            if (b.conventions & (1u << (int)m_convention)) out.push_back(b.chord);
    return out;
}

bool ShortcutMap::rebind(std::string_view id, std::vector<Chord> chords) {
    if (!find(id)) return false;
    for (auto& o : m_overrides)
        if (o.id == id) { o.chords = std::move(chords); return true; }
    m_overrides.push_back({std::string(id), std::move(chords)});
    return true;
}

void ShortcutMap::resetToDefault(std::string_view id) {
    m_overrides.erase(std::remove_if(m_overrides.begin(), m_overrides.end(),
                                     [&](const Override& o) { return o.id == id; }),
                      m_overrides.end());
}

bool ShortcutMap::isOverridden(std::string_view id) const { return findOverride(id) != nullptr; }

std::string_view ShortcutMap::match(Key key, const uiin::Modifiers& mods, bool repeat) const {
    const Chord pressed{key, abstractMods(mods, m_convention)};
    for (const auto& c : m_commands) {
        if (repeat && !c.allowRepeat) continue;
        for (const Chord& ch : chords(c.id))
            if (ch == pressed) return c.id;
    }
    return {};
}

std::vector<ShortcutMap::Conflict> ShortcutMap::conflicts() const {
    std::vector<Conflict> out;
    for (size_t i = 0; i < m_commands.size(); ++i)
        for (const Chord& a : chords(m_commands[i].id))
            for (size_t j = i + 1; j < m_commands.size(); ++j)
                for (const Chord& b : chords(m_commands[j].id))
                    if (a == b) out.push_back({a, m_commands[i].id, m_commands[j].id});
    return out;
}

std::string ShortcutMap::spell(const Chord& c) const {
    const KeyEntry* e = entryFor(c.key);
    std::string s;
    if (m_convention == KeyboardConvention::Apple) {
        // Apple's order: Control, Option, Shift, Command, then the key.
        if (c.mods & ModSecondary) s += "⌃";
        if (c.mods & ModAlt)       s += "⌥";
        if (c.mods & ModShift)     s += "⇧";
        if (c.mods & ModPrimary)   s += "⌘";
        s += e ? (e->apple ? e->apple : e->name) : "?";
    } else {
        if (c.mods & ModPrimary)   s += "Ctrl+";
        if (c.mods & ModSecondary) s += "Win+";
        if (c.mods & ModAlt)       s += "Alt+";
        if (c.mods & ModShift)     s += "Shift+";
        s += e ? (e->pc ? e->pc : e->name) : "?";
    }
    return s;
}

std::string ShortcutMap::spell(std::string_view id) const {
    const auto cs = chords(id);
    return cs.empty() ? std::string() : spell(cs.front());
}

std::string ShortcutMap::overridesToJson() const {
    nlohmann::json bindings = nlohmann::json::object();
    for (const auto& o : m_overrides) {
        nlohmann::json list = nlohmann::json::array();
        for (const Chord& c : o.chords) list.push_back(formatChord(c));
        bindings[o.id] = list;
    }
    return nlohmann::json{{"version", 1}, {"bindings", bindings}}.dump(2);
}

bool ShortcutMap::overridesFromJson(std::string_view text, std::string& error) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, /*allow_exceptions*/ false);
    if (j.is_discarded() || !j.is_object()) { error = "not a JSON object"; return false; }
    if (j.value("version", 0) != 1) { error = "unsupported version (expected 1)"; return false; }
    auto it = j.find("bindings");
    if (it == j.end() || !it->is_object()) { error = "missing \"bindings\" object"; return false; }

    // Parse everything first: a file with one bad chord changes nothing,
    // rather than half of the user's bindings.
    std::vector<Override> parsed;
    for (auto b = it->begin(); b != it->end(); ++b) {
        if (!b.value().is_array()) { error = b.key() + ": expected a list of chords"; return false; }
        Override o{b.key(), {}};
        for (const auto& v : b.value()) {
            if (!v.is_string()) { error = b.key() + ": chords are strings"; return false; }
            auto c = parseChord(v.get<std::string>());
            if (!c) { error = b.key() + ": cannot parse \"" + v.get<std::string>() + "\""; return false; }
            o.chords.push_back(*c);
        }
        parsed.push_back(std::move(o));
    }
    m_overrides = std::move(parsed);
    return true;
}

}  // namespace shortcuts
