// editor_tool_panels_test — the editor's tool panels, without a GUI.
//
// Console, Terminal, Script Viewer, Profiler, Plug-in Manager, Input Bindings
// and Project Settings each have a GUI-free model that the ImGui editor and the
// libgui experiment both draw. This pins what those models decide:
//   - which log lines each console shows (the game-audience rule), Clear, and
//     the error/warning counts;
//   - terminal history walking;
//   - what the highlighter colours;
//   - kit rows' states from the project and the file system;
//   - input.json editing and key capture;
//   - Project Settings' captured key reaching the InputMap;
//   - the frame-time history's order.
#include "editor/panels/console/model.h"
#include "editor/panels/input_bindings/model.h"
#include "editor/panels/plugins/model.h"
#include "editor/panels/profiler/model.h"
#include "editor/panels/project_settings/model.h"
#include "editor/panels/script_viewer/model.h"
#include "editor/panels/terminal/model.h"

#ifdef IMGUI_VERSION
#error "the tool-panel models must not include ImGui: every GUI front end shares them"
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
                   std::printf(__VA_ARGS__);                           \
                   std::printf("\n"); ++g_failures; }                  \
} while (0)

static bool hasLine(const console::LogView::Result& r, const std::string& needle) {
    for (const auto& l : r.lines) if (l.msg.find(needle) != std::string::npos) return true;
    return false;
}

static bool sameColor(edui::Color a, edui::Color b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

int main() {
    elog::setStdout(false);
    const fs::path tmp = fs::temp_directory_path() / "editor_tool_panels_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    // ── 1. Consoles: who sees what ──────────────────────────────────────────
    {
        console::WatchWhileOpen watch;
        watch.update(true);   // engine detail is recorded only while watched
        console::LogView game(console::Audience::Game);
        console::LogView engine(console::Audience::Engine);
        game.clear(); engine.clear();

        LOG_INFO("ToolPanelTestEngine", "engine chatter one");
        LOG_WARN("ToolPanelTestEngine", "engine warning two");
        LOG_ERROR("ToolPanelTestEngine", "engine error three");

        const auto g = game.collect();
        const auto e = engine.collect();
        CHECK(!hasLine(g, "engine chatter one"), "engine Info is NOT for the game builder");
        CHECK(hasLine(g, "engine warning two") && hasLine(g, "engine error three"),
              "warnings and errors from everywhere reach the game console");
        CHECK(g.warnings == 1 && g.errors == 1, "game counts: %u warnings, %u errors", g.warnings, g.errors);
        CHECK(hasLine(e, "engine chatter one"), "the internal console shows engine Info");

        engine.setShowing(elog::Level::Warning, false);
        CHECK(!hasLine(engine.collect(), "engine warning two"), "a level filter hides that level");
        engine.find() = "error three";
        const auto f = engine.collect();
        CHECK(f.lines.size() == 1 && hasLine(f, "error three"), "find narrows to matching lines (%zu)", f.lines.size());

        game.clear();
        CHECK(game.collect().lines.empty(), "Clear moves the view: older lines are gone from it");
        CHECK(sameColor(console::levelColor(elog::Level::Error), edui::Color{1.00f, 0.35f, 0.35f, 1}),
              "errors are red, the same in every console");
        watch.update(false);
    }

    // ── 2. Terminal: output and history ─────────────────────────────────────
    {
        term::TerminalSession t;
        t.setProjectRoot(tmp.string());
        t.runCommand("echo first-command");
        t.runCommand("echo second-command");
        // Compared without trailing spaces: cmd.exe's echo keeps everything up to
        // the redirection, so `echo second-command 2>&1` prints "second-command "
        // on Windows (still failing there after the cd /d fix, WO-038).
        bool sawOutput = false;
        for (std::string l : t.history) {
            while (!l.empty() && l.back() == ' ') l.pop_back();
            sawOutput |= l == "second-command";
        }
        CHECK(sawOutput, "a command's output lands in the history");
        bool changed = false;
        CHECK(t.stepHistory(+1, changed) == "echo second-command" && changed, "Up: the newest command first");
        CHECK(t.stepHistory(+1, changed) == "echo first-command", "Up again: the one before");
        t.stepHistory(+1, changed);
        CHECK(!changed, "Up past the oldest stays put");
        t.stepHistory(-1, changed); t.stepHistory(-1, changed);
        CHECK(t.stepHistory(-1, changed).empty() || !changed, "Down past the newest clears the input");
        CHECK(sameColor(term::TerminalSession::lineColor("$ ls"), edui::Color{0.40f, 0.90f, 0.50f, 1}),
              "commands are green");
    }

#if !defined(_WIN32)
    // ── 2b. Terminal: a project folder the shell would otherwise rewrite ────
    // The root was wrapped in DOUBLE quotes, inside which sh still expands $,
    // backticks and backslashes: a folder named `a $HOME` became a different
    // path, and one containing a backtick ran it. Single-quoted, exactly.
    {
        const fs::path odd = tmp / "it's a $HOME `x` dir";
        fs::create_directories(odd);
        term::TerminalSession t;
        t.setProjectRoot(odd.string());
        t.runCommand("pwd -P");
        const std::string want = fs::canonical(odd).string();
        bool there = false;
        for (const auto& l : t.history) there |= l == want;
        CHECK(there, "a project folder named with ', $ and a backtick is entered exactly (pwd: %s)",
              t.history.size() > 4 ? t.history[4].c_str() : "-");
    }
#endif

    // ── 3. Highlighter ──────────────────────────────────────────────────────
    {
        const auto lines = code::Highlighter::highlight("local x = \"hi\" -- note\nreturn 42", code::Lang::Lua);
        CHECK(lines.size() == 2, "two lines, got %zu", lines.size());
        auto colorOf = [&](size_t line, const std::string& text) {
            for (const auto& s : lines[line]) if (s.text == text) return s.col;
            return edui::Color{-1, -1, -1, -1};
        };
        CHECK(sameColor(colorOf(0, "local"), code::Highlighter::cKeyword()), "a keyword");
        CHECK(sameColor(colorOf(0, "\"hi\""), code::Highlighter::cString()), "a string");
        CHECK(sameColor(colorOf(0, "-- note"), code::Highlighter::cComment()), "a comment");
        CHECK(sameColor(colorOf(1, "42"), code::Highlighter::cNumber()), "a number");
        code::DocSet set;
        std::ofstream(tmp / "a.lua") << "print(1)\n";
        set.open((tmp / "a.lua").string());
        set.open((tmp / "a.lua").string());
        CHECK(set.docs().size() == 1 && set.takeFocus() == (tmp / "a.lua").string(),
              "opening a file twice focuses it instead of opening it again");
        set.docs()[0].open = false; set.prune();
        CHECK(!set.anyOpen(), "a closed document is dropped");
    }

    // ── 4. Kit rows ─────────────────────────────────────────────────────────
    {
        ProjectContext project;
        project.projectRoot = tmp;
        std::ofstream(tmp / "real_kit.so") << "x";
        project.kits.resize(3);
        project.kits[0].name = "off";     project.kits[0].module = "real_kit.so"; project.kits[0].enabled = false;
        project.kits[1].name = "gone";    project.kits[1].module = "nope.so";     project.kits[1].enabled = true;
        project.kits[2].name = "ready";   project.kits[2].module = "real_kit.so"; project.kits[2].enabled = true;
        project.kits[2].requiresKits = {"a", "b"};
        const auto rows = pluginsview::kitRows(project, nullptr, /*simulating*/ false);
        CHECK(rows.size() == 3, "one row per kit");
        CHECK(rows[0].state == pluginsview::KitState::Disabled, "a disabled kit says so");
        CHECK(rows[1].state == pluginsview::KitState::PathNotFound && !rows[1].error.empty(),
              "a missing module is an error with a reason");
        CHECK(rows[2].state == pluginsview::KitState::LoadsAtPlay && rows[2].requiredKits == "a, b",
              "an enabled kit on disk loads at Play, and lists what it needs");
        CHECK(!rows[2].canLoad && !rows[2].canUnload, "load/unload only while playing");
    }

    // ── 5. Input bindings document ──────────────────────────────────────────
    {
        std::ofstream(tmp / "input.json") << R"({"contexts":[{"name":"Gameplay","actions":[
            {"name":"Jump","type":"digital","bindings":["key:Space"]}]}]})";
        bindings::Document doc;
        CHECK(doc.load(tmp) && doc.contextCount() == 1 && doc.actionName(0, 0) == "Jump", "loads");
        const bindings::Slot s0{0, 0, 0};
        CHECK(doc.binding(s0) == "key:Space" && !doc.dirty(), "reads a binding, clean");
        doc.addBinding(0, 0);
        CHECK(doc.bindingCount(0, 0) == 2 && doc.dirty(), "add marks dirty");
        const bindings::Slot s1{0, 0, 1};
        doc.beginCapture(s1);
        CHECK(doc.capturing(s1) && !doc.capturing(s0), "capture aims at one binding");
        CHECK(doc.acceptCapturedKey(Key::W) && doc.binding(s1) == "key:W" && !doc.capturingAny(),
              "the captured key becomes that binding: %s", doc.binding(s1).c_str());
        doc.removeBinding(s0);
        CHECK(doc.bindingCount(0, 0) == 1 && doc.binding(s0) == "key:W", "remove");
        doc.revert();
        CHECK(doc.bindingCount(0, 0) == 1 && doc.binding(s0) == "key:Space" && !doc.dirty(),
              "revert re-reads the file");
        std::ofstream(tmp / "input.json") << "{ not json";
        CHECK(!doc.load(tmp), "a bad file is not loaded");
    }

    // ── 6. Project Settings: a captured key reaches the InputMap ────────────
    {
        auto& map = InputMap::get();
        map.addAction("ToolPanelTestAction");
        StringID act{};
        for (const auto& a : map.actions()) if (a.name == "ToolPanelTestAction") act = a.id;
        ProjectSettingsState s;
        beginCaptureActionKey(s, act);
        CHECK(acceptCapturedKey(s, Key::K) && !s.capturing, "a key ends the capture");
        bool bound = false;
        for (const auto& a : map.actions()) if (a.id == act) for (Key k : a.keys) bound |= k == Key::K;
        CHECK(bound, "and is bound to the action");
        beginCaptureActionKey(s, act);
        CHECK(acceptCapturedKey(s, Key::Escape) && !s.capturing, "Escape cancels");
        size_t keys = 0;
        for (const auto& a : map.actions()) if (a.id == act) keys = a.keys.size();
        CHECK(keys == 1, "and binds nothing (%zu keys)", keys);
        CHECK(!acceptCapturedKey(s, Key::J), "with no capture waiting, a key does nothing");
        map.removeAction(act);
    }

    // ── 7. Frame-time history: oldest first ─────────────────────────────────
    {
        profview::FrameHistory h;
        for (int i = 0; i < profview::FrameHistory::kSize + 5; ++i) h.push((float)i);
        CHECK(h.at(0) == 5.0f && h.at(profview::FrameHistory::kSize - 1) == (float)(profview::FrameHistory::kSize + 4),
              "the oldest kept sample first, newest last: %f .. %f", h.at(0), h.at(profview::FrameHistory::kSize - 1));
    }

    fs::remove_all(tmp, ec);
    if (g_failures) {
        std::printf("\neditor_tool_panels_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("editor_tool_panels_test: all checks passed\n");
    return 0;
}
