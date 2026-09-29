#pragma once
// ── shortcuts — editor commands and their key chords, on any platform ────────
//
// Why this exists: the editor spelled its shortcuts as "is the left Super key
// down and was S pressed", which is Cmd+S on a Mac and Win+S — the Start
// search — on Windows and Linux. Save and Undo simply did not work off macOS,
// and every new shortcut would have repeated the mistake.
//
// So a shortcut is written ABSTRACTLY and resolved per keyboard convention:
//
//   Primary    the command modifier: Cmd on Apple, Ctrl on PC
//   Secondary  the other one:        Ctrl on Apple, Super/Win on PC
//   Shift, Alt as themselves (Alt is Option on Apple)
//
// and where the platforms genuinely DISAGREE rather than just swapping a key
// (Redo is Cmd+Shift+Z on a Mac but Ctrl+Y on Windows), a command lists a
// default per convention. A new OS reports the convention it follows
// (IPlatform::keyboardConvention); if it follows neither, it is a new row in
// uiin::KeyboardConvention and in the defaults — not a new code path.
//
// GUI-free on purpose: the ImGui editor and the libgui experiment both drive
// one ShortcutMap, so a binding is defined, rebound and spelled once.
// Keys are PHYSICAL positions (engine Key), as everywhere in the engine.
#include "runtime/input/input_event.h"
#include "runtime/platform/ui_input.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shortcuts {

using uiin::KeyboardConvention;

enum Mod : uint8_t {
    ModNone      = 0,
    ModShift     = 1 << 0,
    ModAlt       = 1 << 1,
    ModPrimary   = 1 << 2,
    ModSecondary = 1 << 3,
};

struct Chord {
    Key     key  = Key::Unknown;
    uint8_t mods = ModNone;
    bool operator==(const Chord&) const = default;
    bool valid() const { return key != Key::Unknown; }
};

// Which conventions a default binding applies to.
enum ConventionMask : uint8_t {
    OnApple = 1 << (int)KeyboardConvention::Apple,
    OnPc    = 1 << (int)KeyboardConvention::Pc,
    OnAll   = OnApple | OnPc,
};

struct Binding {
    uint8_t conventions = OnAll;
    Chord   chord;
};

struct CommandSpec {
    std::string          id;        // stable, persisted: "file.save"
    std::string          label;     // for menus and the bindings UI: "Save Scene"
    std::vector<Binding> defaults;
    bool                 allowRepeat = false;   // fire again on key auto-repeat
};

// Physical modifier state -> abstract bits, for a convention.
uint8_t abstractMods(const uiin::Modifiers& m, KeyboardConvention c);

// Text form, persisted in overrides and typed by users: "Primary+Shift+Z".
// Accepts Primary, Secondary, Shift, Alt, then one key name (keyName()).
std::optional<Chord> parseChord(std::string_view text);
std::string          formatChord(const Chord& chord);

// Canonical key names used by parseChord/formatChord ("S", "F5", "Comma",
// "PageUp", "Kp7"...). nullptr for Key::Unknown or unnamed keys.
const char*          keyName(Key k);
std::optional<Key>   keyFromName(std::string_view name);

class ShortcutMap {
public:
    explicit ShortcutMap(KeyboardConvention c = uiin::defaultKeyboardConvention())
        : m_convention(c) {}

    void               setConvention(KeyboardConvention c) { m_convention = c; }
    KeyboardConvention convention() const { return m_convention; }

    // False if the id is already defined (ids are persisted; two meanings for
    // one id would silently rebind the wrong command).
    bool define(CommandSpec spec);
    bool has(std::string_view id) const { return find(id) != nullptr; }
    const std::vector<CommandSpec>& commands() const { return m_commands; }

    // The chords that trigger `id` NOW: the user's override if there is one,
    // else the defaults for the current convention.
    std::vector<Chord> chords(std::string_view id) const;

    // A user rebinding. An empty list unbinds. False for an unknown id.
    bool rebind(std::string_view id, std::vector<Chord> chords);
    void resetToDefault(std::string_view id);
    bool isOverridden(std::string_view id) const;

    // The command a key press triggers, or empty. Modifiers must match
    // EXACTLY (Ctrl+Shift+S is not Ctrl+S). Repeats only match commands that
    // allow them. If two commands share a chord, the first defined wins and
    // conflicts() reports it.
    std::string_view match(Key key, const uiin::Modifiers& mods, bool repeat = false) const;

    struct Conflict { Chord chord; std::string first, second; };
    std::vector<Conflict> conflicts() const;

    // Menu spelling in the platform's own style: "⇧⌘Z" on Apple,
    // "Ctrl+Shift+Z" on PC. The id form spells the first chord, or "".
    std::string spell(const Chord& chord) const;
    std::string spell(std::string_view id) const;

    // User overrides only (defaults live in code), as JSON:
    //   {"version":1,"bindings":{"edit.redo":["Primary+Shift+Z"],"file.save":[]}}
    // Unknown ids are kept, not dropped: a binding for a command from a plugin
    // that is not loaded today must survive a save. Bad chords are errors.
    std::string overridesToJson() const;
    bool        overridesFromJson(std::string_view json, std::string& error);

private:
    struct Override { std::string id; std::vector<Chord> chords; };
    const CommandSpec* find(std::string_view id) const;
    const Override*    findOverride(std::string_view id) const;

    KeyboardConvention       m_convention;
    std::vector<CommandSpec> m_commands;
    std::vector<Override>    m_overrides;
};

}  // namespace shortcuts
