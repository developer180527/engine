// editor_shortcuts_test — editor commands resolve per keyboard convention.
//
// The bug this exists for: the editor's shortcuts were "LeftSuper + key", so
// Save/Undo/Redo were Cmd on a Mac and the Windows key everywhere else — dead
// off macOS. editor/core/shortcuts.h writes chords abstractly (Primary = Cmd
// or Ctrl) and resolves them per convention. This pins that resolution, the
// places the conventions genuinely differ (Redo, Delete), the text format
// users edit, and the override file.
#include "editor/core/editor_commands.h"

#include <cstdio>
#include <string>

using namespace shortcuts;
using uiin::KeyboardConvention;
using uiin::Modifiers;

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
                   std::printf(__VA_ARGS__);                           \
                   std::printf("\n"); ++g_failures; }                  \
} while (0)

static Modifiers mods(bool shift, bool ctrl, bool alt, bool super) {
    Modifiers m; m.shift = shift; m.ctrl = ctrl; m.alt = alt; m.super = super; return m;
}
static const Modifiers kNone  = mods(false, false, false, false);
static const Modifiers kCmd   = mods(false, false, false, true);
static const Modifiers kCtrl  = mods(false, true,  false, false);
static const Modifiers kCmdSh = mods(true,  false, false, true);
static const Modifiers kCtlSh = mods(true,  true,  false, false);

static std::string m(const ShortcutMap& map, Key k, const Modifiers& md, bool repeat = false) {
    return std::string(map.match(k, md, repeat));
}

int main() {
    ShortcutMap apple(KeyboardConvention::Apple);
    ShortcutMap pc(KeyboardConvention::Pc);
    defineEditorCommands(apple);
    defineEditorCommands(pc);

    // ── 1. Primary is Cmd on Apple and Ctrl on PC — the original bug ────────
    CHECK(m(apple, Key::S, kCmd)  == cmd::Save, "Cmd+S saves on Apple");
    CHECK(m(apple, Key::S, kCtrl).empty(),      "Ctrl+S does nothing on Apple");
    CHECK(m(pc,    Key::S, kCtrl) == cmd::Save, "Ctrl+S saves on PC");
    CHECK(m(pc,    Key::S, kCmd).empty(),       "Win+S must NOT save on PC (it was the old behaviour)");
    CHECK(abstractMods(kCmd, KeyboardConvention::Apple) == ModPrimary, "Cmd is Primary on Apple");
    CHECK(abstractMods(kCmd, KeyboardConvention::Pc) == ModSecondary, "Win is Secondary on PC");

    // ── 2. Modifiers match exactly ──────────────────────────────────────────
    CHECK(m(apple, Key::Z, kCmd)   == cmd::Undo, "Cmd+Z undoes");
    CHECK(m(apple, Key::Z, kCmdSh) == cmd::Redo, "Cmd+Shift+Z redoes, not undoes");
    CHECK(m(apple, Key::Z, kNone).empty(),       "a bare Z is not a command");

    // ── 3. Where the platforms really differ ────────────────────────────────
    CHECK(m(pc, Key::Y, kCtrl)  == cmd::Redo, "Ctrl+Y redoes on PC");
    CHECK(m(pc, Key::Z, kCtlSh) == cmd::Redo, "Ctrl+Shift+Z also redoes on PC");
    CHECK(m(apple, Key::Y, kCmd).empty(),     "Cmd+Y is not Redo on Apple");
    CHECK(m(apple, Key::Backspace, kNone) == cmd::Delete, "a Mac's delete key (Backspace) deletes");
    CHECK(m(pc, Key::Backspace, kNone).empty(),           "Backspace does not delete entities on PC");
    CHECK(m(pc, Key::Delete, kNone) == cmd::Delete,       "Delete deletes on PC");

    // ── 4. Auto-repeat only fires commands that ask for it ──────────────────
    CHECK(m(apple, Key::Z, kCmd, /*repeat*/ true).empty(), "holding Cmd+Z does not repeat Undo");

    // ── 5. Menu spelling per convention ─────────────────────────────────────
    CHECK(apple.spell(std::string_view(cmd::Redo)) == "⇧⌘Z", "Apple Redo: %s", apple.spell(std::string_view(cmd::Redo)).c_str());
    CHECK(pc.spell(std::string_view(cmd::Redo)) == "Ctrl+Y", "PC Redo spells its first chord: %s", pc.spell(std::string_view(cmd::Redo)).c_str());
    CHECK(apple.spell(std::string_view(cmd::Settings)) == "⌘,", "Apple settings: %s", apple.spell(std::string_view(cmd::Settings)).c_str());
    CHECK(pc.spell(std::string_view(cmd::Settings)) == "Ctrl+,", "PC settings: %s", pc.spell(std::string_view(cmd::Settings)).c_str());
    CHECK(apple.spell(Chord{Key::S, ModPrimary | ModSecondary | ModAlt | ModShift}) == "⌃⌥⇧⌘S",
          "Apple modifier order is Control, Option, Shift, Command");

    // ── 6. The defaults conflict on neither convention ──────────────────────
    CHECK(apple.conflicts().empty(), "Apple defaults conflict (%zu)", apple.conflicts().size());
    CHECK(pc.conflicts().empty(),    "PC defaults conflict (%zu)", pc.conflicts().size());
    CHECK(!apple.define({cmd::Save, "again", {}}), "a duplicate id is refused");

    // ── 7. The text format round-trips every named key ──────────────────────
    int named = 0;
    for (int k = 0; k <= kKeyCodeMax; ++k) {
        const char* n = keyName((Key)k);
        if (!n) continue;
        ++named;
        Chord c{(Key)k, ModPrimary | ModShift};
        auto back = parseChord(formatChord(c));
        CHECK(back && *back == c, "round trip failed for %s", n);
    }
    // Every Key except the eight modifier keys, which a chord never names as
    // its key. A new Key without a name fails here.
    CHECK(named == 93, "every non-modifier Key needs a name: %d named", named);
    for (const char* bad : {"", "Primary+", "+S", "Ctrl+S", "Cmd+S", "Shift+Shift+S", "Primary+Nope", "Primary++"})
        CHECK(!parseChord(bad), "\"%s\" must not parse", bad);
    CHECK(parseChord(" primary + shift + z ") == (std::optional<Chord>(Chord{Key::Z, ModPrimary | ModShift})),
          "names are case-insensitive and spaces are allowed");

    // ── 8. Overrides: rebind, conflict, persist, reset ──────────────────────
    {
        ShortcutMap map(KeyboardConvention::Pc);
        defineEditorCommands(map);
        CHECK(map.rebind(cmd::Copy, {Chord{Key::S, ModPrimary}}), "rebind");
        CHECK(map.conflicts().size() == 1, "a rebinding onto Save is reported as a conflict");
        CHECK(m(map, Key::S, kCtrl) == cmd::Save, "on a conflict the first defined command wins");
        CHECK(map.rebind(cmd::Undo, {}), "unbind");
        CHECK(m(map, Key::Z, kCtrl).empty(), "an unbound command no longer fires");
        CHECK(!map.rebind("no.such.command", {}), "rebinding an unknown id is refused");

        const std::string json = map.overridesToJson();
        ShortcutMap fresh(KeyboardConvention::Pc);
        defineEditorCommands(fresh);
        std::string err;
        CHECK(fresh.overridesFromJson(json, err), "own output reloads: %s", err.c_str());
        CHECK(m(fresh, Key::Z, kCtrl).empty() && fresh.isOverridden(cmd::Copy), "overrides survive a round trip");

        // A command from a plugin that is not loaded keeps its binding.
        CHECK(fresh.overridesFromJson(R"({"version":1,"bindings":{"plugin.thing":["Alt+F5"]}})", err),
              "unknown ids load: %s", err.c_str());
        CHECK(fresh.overridesToJson().find("plugin.thing") != std::string::npos, "and are saved back");

        // One bad chord rejects the whole file and changes nothing.
        CHECK(!fresh.overridesFromJson(R"({"version":1,"bindings":{"edit.undo":["Ctrl+Z"]}})", err),
              "a physical modifier name is an error");
        CHECK(fresh.overridesToJson().find("plugin.thing") != std::string::npos, "a rejected file leaves overrides alone");
        CHECK(!fresh.overridesFromJson("[1,2]", err) && !fresh.overridesFromJson(R"({"version":2,"bindings":{}})", err),
              "wrong shape and wrong version are errors");

        map.resetToDefault(cmd::Undo);
        CHECK(m(map, Key::Z, kCtrl) == cmd::Undo, "reset restores the default");
    }

    if (g_failures) {
        std::printf("\neditor_shortcuts_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("editor_shortcuts_test: all checks passed\n");
    return 0;
}
