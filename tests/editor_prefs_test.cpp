// ── editor_prefs_test — editor.json is a file, therefore it is untrusted ─────
// `EditorPrefs` persists the editor's camera and selection to `editor.json` in
// the project root. That makes it the one piece of editor state that comes back
// FROM DISK, which puts it in the same category as the scene deserializers: a
// text file that gets hand-edited, half-written when a machine dies mid-save,
// and copied between machines.
//
// Its `load()` is wrapped in `try { ... } catch (...) {}`, which reads as
// total safety and is not. nlohmann's CONST `operator[](size_type)` is
// UNDEFINED BEHAVIOUR out of range — it is not `at()`, there is no bounds
// check, and there is no exception for the catch to catch. So
// `jc["position"][0..2]` on a two-element array is a null-json dereference that
// sails straight past the handler.
//
// This is the third instance of that exact defect found in one pass: five sites
// in `entity_serializer.h` (fixed, with a fuzz corpus), one in
// `UndoStack::desTf`, and this. Same helper fixes all of them.
//
// Headless: no window, no GPU. EditorCamera's `edwin::` calls are never
// referenced here, so the window-ops TU is not needed to link.
#include <cstdio>
#include <cmath>
#include <fstream>
#include <string>

#include "editor/editor_prefs.h"

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

static fs::path g_root;

static void writePrefs(const std::string& body) {
    std::ofstream f(g_root / "editor.json", std::ios::trunc);
    f << body;
}

// Every load starts from a KNOWN camera, so "unchanged" is distinguishable
// from "overwritten with zeros" — a distinction that matters, because a
// silently zeroed camera looks like a working load until you notice the view
// jumped to the origin.
static EditorCamera knownCamera() {
    EditorCamera c;
    c.position = { 11.0f, 22.0f, 33.0f };
    c.yaw      = 0.5f;
    c.pitch    = 0.25f;
    return c;
}

static bool isKnown(const EditorCamera& c) {
    return std::fabs(c.position.x - 11.0f) < 1e-4f
        && std::fabs(c.position.y - 22.0f) < 1e-4f
        && std::fabs(c.position.z - 33.0f) < 1e-4f;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("editor_prefs_test\n");

    g_root = fs::temp_directory_path() / "engine-editor-prefs-test";
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);

    // ── Round-trip ──────────────────────────────────────────────────────────
    {
        EditorCamera out;
        out.position = { 1.5f, -2.5f, 3.5f };
        out.yaw      = 1.25f;
        out.pitch    = -0.75f;
        EditorPrefs::save(g_root, out, "Player");

        EditorCamera in = knownCamera();
        std::string sel;
        EditorPrefs::load(g_root, in, sel);

        CHECK(std::fabs(in.position.x - 1.5f) < 1e-4f
              && std::fabs(in.position.y + 2.5f) < 1e-4f
              && std::fabs(in.position.z - 3.5f) < 1e-4f,
              "camera position round-trips (%.2f %.2f %.2f)",
              in.position.x, in.position.y, in.position.z);
        CHECK(std::fabs(in.yaw - 1.25f) < 1e-4f
              && std::fabs(in.pitch + 0.75f) < 1e-4f,
              "yaw and pitch round-trip (%.2f %.2f)", in.yaw, in.pitch);
        CHECK(sel == "Player", "the selected entity name round-trips (\"%s\")",
              sel.c_str());
    }

    // ── No file: leave the caller's defaults alone ──────────────────────────
    {
        fs::remove(g_root / "editor.json", ec);
        EditorCamera in = knownCamera();
        std::string sel = "untouched";
        EditorPrefs::load(g_root, in, sel);
        CHECK(isKnown(in), "a missing editor.json leaves the camera untouched");
        CHECK(sel == "untouched", "...and the selection untouched");
    }

    // ── Malformed and hostile files ─────────────────────────────────────────
    // Each of these must leave the camera at its defaults and, above all, must
    // not take the editor down on startup — this runs during project open, so a
    // crash here means the project cannot be opened at all.
    {
        struct Case { const char* what; const char* body; };
        const Case cases[] = {
            { "not JSON at all",              "}{ this is not json" },
            { "an empty file",                "" },
            { "a bare array",                 "[1,2,3]" },
            { "a bare number",                "42" },
            { "camera as a string",           R"({"camera":"nope"})" },
            { "position as a string",         R"({"camera":{"position":"nope"}})" },
            { "position as an object",        R"({"camera":{"position":{"x":1}}})" },
            { "yaw as a string",              R"({"camera":{"yaw":"1.0"}})" },
            { "selectedEntity as a number",   R"({"selectedEntity":42})" },
            // ── The UB case ─────────────────────────────────────────────────
            // A short position array. One dropped element from a merge, a hand
            // edit, or a truncated write. `[0]`, `[1]`, `[2]` on a two-element
            // array indexes past the underlying vector.
            { "position with TWO elements",   R"({"camera":{"position":[1.0,2.0]}})" },
            { "position with ONE element",    R"({"camera":{"position":[1.0]}})" },
            { "position EMPTY",               R"({"camera":{"position":[]}})" },
            { "position of wrong-typed items", R"({"camera":{"position":["a","b","c"]}})" },
            { "position with nulls",          R"({"camera":{"position":[null,null,null]}})" },
        };

        for (const auto& c : cases) {
            writePrefs(c.body);
            EditorCamera in = knownCamera();
            std::string sel = "untouched";
            bool threw = false;
            try { EditorPrefs::load(g_root, in, sel); }
            catch (...) { threw = false; /* caught internally is fine */ }
            // Reaching here at all is the assertion: the UB cases crash the
            // process rather than throwing, so a returning call IS the pass.
            CHECK(!threw && std::isfinite(in.position.x)
                  && std::isfinite(in.position.y)
                  && std::isfinite(in.position.z)
                  && std::isfinite(in.yaw) && std::isfinite(in.pitch),
                  "%s: survives with a finite camera", c.what);
        }
    }

    // ── A partially valid file keeps what it can ────────────────────────────
    // Dropping the whole file because one field is malformed would lose the
    // camera every time someone fat-fingers the selection.
    {
        writePrefs(R"({"camera":{"position":[4.0,5.0,6.0],"yaw":"bad"},
                       "selectedEntity":"Keep"})");
        EditorCamera in = knownCamera();
        std::string sel;
        EditorPrefs::load(g_root, in, sel);
        CHECK(std::isfinite(in.position.x) && std::isfinite(in.yaw),
              "a partially malformed file still yields a usable camera");
    }

    fs::remove_all(g_root, ec);

    if (g_failures) {
        std::printf("editor_prefs_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("editor_prefs_test: PASS\n");
    return 0;
}
