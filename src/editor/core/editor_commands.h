#pragma once
// ── editor_commands — the editor's commands and their default chords ─────────
// One list for every editor front end. The ids are persisted in the user's
// shortcut overrides, so they are stable: rename a label freely, never an id.
#include "editor/core/shortcuts.h"

namespace cmd {

inline constexpr const char* Save        = "file.save";
inline constexpr const char* Quit        = "app.quit";
inline constexpr const char* Undo        = "edit.undo";
inline constexpr const char* Redo        = "edit.redo";
inline constexpr const char* Cut         = "edit.cut";
inline constexpr const char* Copy        = "edit.copy";
inline constexpr const char* Paste       = "edit.paste";
inline constexpr const char* Duplicate   = "edit.duplicate";
inline constexpr const char* Delete      = "edit.delete";
inline constexpr const char* SelectAll   = "edit.select_all";
inline constexpr const char* Settings    = "app.project_settings";
inline constexpr const char* PlayPause   = "play.toggle";

}  // namespace cmd

namespace shortcuts {

inline void defineEditorCommands(ShortcutMap& m) {
    using namespace shortcuts;
    auto on = [](uint8_t where, Key k, uint8_t mods = ModNone) { return Binding{where, Chord{k, mods}}; };

    m.define({cmd::Save,      "Save Scene",        {on(OnAll, Key::S, ModPrimary)}});
    m.define({cmd::Quit,      "Quit",              {on(OnAll, Key::Q, ModPrimary)}});
    m.define({cmd::Undo,      "Undo",              {on(OnAll, Key::Z, ModPrimary)}});
    // Where the platforms disagree rather than swap a key: Apple has one Redo,
    // Windows and Linux editors accept both.
    m.define({cmd::Redo,      "Redo",              {on(OnApple, Key::Z, ModPrimary | ModShift),
                                                    on(OnPc,    Key::Y, ModPrimary),
                                                    on(OnPc,    Key::Z, ModPrimary | ModShift)}});
    m.define({cmd::Cut,       "Cut",               {on(OnAll, Key::X, ModPrimary)}});
    m.define({cmd::Copy,      "Copy",              {on(OnAll, Key::C, ModPrimary)}});
    m.define({cmd::Paste,     "Paste",             {on(OnAll, Key::V, ModPrimary)}});
    m.define({cmd::Duplicate, "Duplicate",         {on(OnAll, Key::D, ModPrimary)}});
    // A Mac keyboard's "delete" key is Backspace by position; the forward
    // Delete key is missing from most of them.
    m.define({cmd::Delete,    "Delete",            {on(OnApple, Key::Backspace),
                                                    on(OnAll,   Key::Delete)}});
    m.define({cmd::SelectAll, "Select All",        {on(OnAll, Key::A, ModPrimary)}});
    m.define({cmd::Settings,  "Project Settings…", {on(OnAll, Key::Comma, ModPrimary)}});
    m.define({cmd::PlayPause, "Play / Pause",      {on(OnAll, Key::P, ModPrimary)}});
}

}  // namespace shortcuts
