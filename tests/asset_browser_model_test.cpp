// asset_browser_model_test — the asset browser's behaviour, without a GUI.
//
// AssetBrowserModel (src/editor/panels/asset_browser/model.h) is what the
// ImGui panel draws and what a second front end (the libgui experiment) will
// draw too, so the two cannot disagree about what a click does. This pins
// that behaviour: what a folder lists and in what order, what "open" means for
// each kind of entry, and what each file operation does to the selection and
// the rescan. It runs on a scratch directory; nothing here needs a window.
//
// It also pins that the model is GUI-free: the first check below fails the
// BUILD if model.h ever pulls ImGui back in.
#include "editor/panels/asset_browser/model.h"

#ifdef IMGUI_VERSION
#error "asset_browser/model.h must not include ImGui: it is shared by every GUI front end"
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

static void touch(const fs::path& p, const char* text = "x") {
    std::ofstream(p, std::ios::binary) << text;
}

// A fresh project: <root>/assets with two folders, two files and OS junk.
static fs::path makeProject() {
    fs::path root = fs::temp_directory_path() / "asset_browser_model_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "assets" / "models");
    fs::create_directories(root / "assets" / "Scripts");
    touch(root / "assets" / "zeta.lua", "-- lua");
    touch(root / "assets" / "alpha.bin");
    touch(root / "assets" / ".DS_Store");
    touch(root / "assets" / "._alpha.bin");
    return root;
}

int main() {
    const fs::path project = makeProject();
    const fs::path assets  = project / "assets";
    ImporterRegistry importers;   // empty: nothing is cookable, which is fine here
    ab::AssetBrowserModel m;

    auto rescan = [&] { m.update(importers, nullptr, project, nullptr); };

    // ── 1. Listing: folders first, then by name; OS junk never shows ────────
    m.syncRoot(assets);
    CHECK(m.needsRefresh(), "a new root must rescan");
    rescan();
    CHECK(!m.needsRefresh(), "update() must consume the rescan");
    {
        const auto& f = m.files();
        CHECK(f.size() == 4, "4 entries expected (junk skipped), got %zu", f.size());
        if (f.size() == 4) {
            CHECK(f[0].isDir && f[0].name == "Scripts", "first: Scripts/, got %s", f[0].name.c_str());
            CHECK(f[1].isDir && f[1].name == "models",  "second: models/, got %s", f[1].name.c_str());
            CHECK(f[2].name == "alpha.bin" && f[3].name == "zeta.lua",
                  "files by name: got %s, %s", f[2].name.c_str(), f[3].name.c_str());
            CHECK(f[3].ext == ".lua", "ext is lowercased with the dot: %s", f[3].ext.c_str());
        }
        for (const auto& e : f)
            CHECK(!ab::isOsJunk(e.name), "junk listed: %s", e.name.c_str());
    }
    CHECK(m.breadcrumb() == "assets", "root breadcrumb: %s", m.breadcrumb().c_str());

    // ── 2. Opening: folder enters, text views, cookable spawns, other none ──
    {
        const ab::FileEntry models = m.files()[1];
        m.select(3);
        ab::OpenRequest r = m.open(models);
        CHECK(r.kind == ab::OpenKind::Navigated, "a folder opens by navigating");
        CHECK(m.currentDir() == assets / "models", "now in models/");
        CHECK(m.breadcrumb() == "assets / models", "breadcrumb: %s", m.breadcrumb().c_str());
        // Opening only MARKS the rescan: the list a front end is iterating
        // this frame is still the old one.
        CHECK(m.needsRefresh() && m.files().size() == 4, "list must survive until update()");
        CHECK(m.selectedIndex() == 3, "until the rescan, the selection still points into the old list");
        rescan();
        CHECK(m.files().empty(), "models/ is empty");
        // The bug this pins: selection was an INDEX, so row 3 of the parent
        // became "whatever is row 3 here". It is a path now.
        CHECK(m.selectedIndex() == -1 && m.selected() == nullptr,
              "a file from another folder must not appear selected here");
        m.navigate(assets);
        rescan();
    }
    {
        ab::FileEntry lua = m.files()[3];
        ab::OpenRequest r = m.open(lua);
        CHECK(r.kind == ab::OpenKind::ViewText && r.path == lua.fullPath, "a .lua opens as text");

        ab::FileEntry bin = m.files()[2];
        CHECK(m.open(bin).kind == ab::OpenKind::None, "an unknown binary opens as nothing");

        ab::FileEntry mesh = bin;
        mesh.supported = true;
        r = m.open(mesh);
        CHECK(r.kind == ab::OpenKind::Spawn && r.entry == &mesh, "a cookable file is handed back to spawn");
        CHECK(!m.needsRefresh(), "opening a file does not rescan");
    }

    // ── 3. Context-menu "Open" re-finds the entry by path ───────────────────
    {
        m.setActionTarget(m.files()[1]);             // models/
        CHECK(m.openActionTarget().kind == ab::OpenKind::Navigated, "menu Open on a folder navigates");
        m.navigate(assets);
        rescan();
        m.setActionTarget(m.files()[3]);             // zeta.lua
        ab::OpenRequest r = m.openActionTarget();
        CHECK(r.kind == ab::OpenKind::ViewText && r.path == (assets / "zeta.lua").string(),
              "menu Open on a script views it");
    }

    // ── 4. File operations: each rescans; rename/delete drop the selection ──
    {
        CHECK(!m.createInCurrentDir(ab::NewKind::Folder, "", nullptr), "an empty name is refused");
        CHECK(!m.needsRefresh(), "a refused create does not rescan");

        m.select(2);   // alpha.bin
        CHECK(m.createInCurrentDir(ab::NewKind::Folder, "textures", nullptr), "create folder");
        CHECK(fs::is_directory(assets / "textures"), "folder exists on disk");
        CHECK(m.needsRefresh(), "create rescans");
        rescan();
        // The second incident: the new folder sorts ABOVE alpha.bin, pushing it
        // from row 2 to row 3. An index-based selection moved onto the folder.
        CHECK(m.selected() && m.selected()->name == "alpha.bin",
              "the selection follows its file across a rescan, got %s",
              m.selected() ? m.selected()->name.c_str() : "(none)");
        CHECK(m.selectedIndex() == 3, "alpha.bin is now row 3, got %d", m.selectedIndex());
        m.select(99);
        CHECK(m.selectedIndex() == -1 && m.selectedPath().empty(), "an out-of-range select selects nothing");

        CHECK(m.createInCurrentDir(ab::NewKind::ScriptLua, "Player", nullptr), "create script");
        CHECK(fs::exists(assets / "Player.lua"), "script gets its extension");
        CHECK(!m.createInCurrentDir(ab::NewKind::ScriptLua, "Player", nullptr), "an existing script is not clobbered");
        rescan();

        // alpha.bin: after the two folders and textures/, it is index 3.
        ab::FileEntry alpha;
        for (const auto& e : m.files()) if (e.name == "alpha.bin") alpha = e;
        m.setActionTarget(alpha);
        m.duplicateTarget(nullptr);
        CHECK(fs::exists(assets / "alpha copy.bin"), "duplicate makes 'alpha copy.bin'");
        CHECK(m.needsRefresh(), "duplicate rescans");
        rescan();

        m.select(1);
        CHECK(m.renameTarget("beta.bin", nullptr), "rename");
        CHECK(fs::exists(assets / "beta.bin") && !fs::exists(assets / "alpha.bin"), "renamed on disk");
        CHECK(m.selectedIndex() == -1, "rename clears the selection");
        rescan();

        ab::FileEntry beta;
        for (const auto& e : m.files()) if (e.name == "beta.bin") beta = e;
        m.setActionTarget(beta);
        m.select(1);
        CHECK(m.deleteTarget(nullptr), "delete");
        CHECK(!fs::exists(assets / "beta.bin"), "deleted on disk");
        CHECK(m.selectedIndex() == -1 && m.selected() == nullptr, "delete clears the selection");
        rescan();
    }

    // ── 5. Loaded flags refresh without a rescan ────────────────────────────
    {
        m.update(importers, nullptr, project,
                 [](const std::string& p) { return p.find("zeta.lua") != std::string::npos; });
        int loaded = 0;
        for (const auto& e : m.files()) loaded += e.loaded;
        CHECK(loaded == 1, "exactly zeta.lua reports loaded, got %d", loaded);
    }

    // ── 6. A different project resets to its root ───────────────────────────
    {
        m.navigate(assets / "Scripts");
        m.syncRoot(assets);
        CHECK(m.currentDir() == assets / "Scripts", "the same root keeps the current folder");
        fs::create_directories(project / "other");
        m.syncRoot(project / "other");
        CHECK(m.currentDir() == project / "other" && m.needsRefresh(), "a new root resets and rescans");
    }

    // ── 7. The view mode toggles ────────────────────────────────────────────
    CHECK(m.viewMode() == ab::ViewMode::Grid, "grid by default");
    m.toggleViewMode();
    CHECK(m.viewMode() == ab::ViewMode::List, "toggles to list");

    std::error_code ec;
    fs::remove_all(project, ec);
    if (g_failures) {
        std::printf("\nasset_browser_model_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("asset_browser_model_test: all checks passed\n");
    return 0;
}
