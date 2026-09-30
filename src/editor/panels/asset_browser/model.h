#pragma once
// ── AssetBrowserModel — the asset browser without its GUI ───────────────────
//
// Everything the browser KNOWS and DOES: which folder it is in, what that
// folder contains, what is selected, which entry a context menu is acting on,
// and the operations on them (navigate, open, create, rename, delete,
// duplicate). No drawing and no GUI toolkit: panel.h draws it with ImGui, and a
// second front end (the libgui experiment) can drive the same model.
//
// What stays in a front end: widget-only state — edit buffers, which modal is
// open, the code viewer — and the calls that need the engine or the OS to act
// on an entry the model has resolved (spawning a mesh, showing a text file).
// open() says which of those to do rather than doing it, so the model is
// testable without an engine context.
#include "types.h"
#include "scan.h"
#include "actions.h"
#include "assets/cookers/cook_service.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ab {

// What opening an entry means. Directories are handled by the model itself;
// the rest are for the front end to carry out.
enum class OpenKind { None, Navigated, Spawn, ViewText };

struct OpenRequest {
    OpenKind         kind  = OpenKind::None;
    const FileEntry* entry = nullptr;   // Spawn: the entry (valid until the next scan)
    std::string      path;              // ViewText: the file to show
};

class AssetBrowserModel {
public:
    // ── Where we are ────────────────────────────────────────────────────────
    // Follows the project: a changed assets root resets to that root.
    void syncRoot(const std::filesystem::path& assetsRoot) {
        if (m_root.empty() || m_root != assetsRoot) {
            m_root        = assetsRoot;
            m_currentDir  = m_root;
            m_needRefresh = true;
        }
    }
    const std::filesystem::path& root()       const { return m_root; }
    const std::filesystem::path& currentDir() const { return m_currentDir; }

    // "assets" or "assets / a/b", relative to the root.
    std::string breadcrumb() const {
        std::error_code ec;
        auto rel = std::filesystem::relative(m_currentDir, m_root, ec);
        std::string crumb = "assets";
        if (!ec && rel != ".") crumb += " / " + rel.generic_string();
        return crumb;
    }

    // The selection is kept by PATH (see select()), so entering a folder
    // leaves nothing selected there unless the selected file is in it.
    void navigate(const std::filesystem::path& dir) {
        m_currentDir  = dir;
        m_needRefresh = true;
    }

    // ── What is here ────────────────────────────────────────────────────────
    // Rescan the folder if something asked for it; otherwise only refresh each
    // file's `loaded` flag, which changes as loads finish and is cheap to ask.
    void update(const SpawnableFn&                             spawnable,
                assetlib::AssetRegistry*                       registry,
                const std::filesystem::path&                   projectRoot,
                const std::function<bool(const std::string&)>& isLoaded) {
        if (m_needRefresh) {
            m_files = scanDir(m_currentDir, spawnable, registry, projectRoot,
                              projectRoot / ".cache");
            m_needRefresh = false;
            resolveSelection();
        } else if (isLoaded) {
            for (auto& f : m_files)
                if (!f.isDir) f.loaded = isLoaded(f.fullPath);
        }
    }

    // Rescan on the next update, and ask the cooker to look again too.
    void requestRefresh(CookService* cook) {
        m_needRefresh = true;
        if (cook) cook->requestRefresh();
    }
    void markDirty() { m_needRefresh = true; }
    bool needsRefresh() const { return m_needRefresh; }

    const std::vector<FileEntry>& files() const { return m_files; }

    // ── Selection ───────────────────────────────────────────────────────────
    // The selection is a PATH; the index is only where that path sits in the
    // current listing. It used to be the index itself, so every rescan
    // (entering a folder, creating or duplicating a file, Refresh) left it on
    // whatever entry now occupied that slot: a double-click into a folder
    // showed a file nobody clicked as selected, and creating a folder moved
    // the selection onto the new folder. A path that is no longer listed
    // selects nothing.
    int  selectedIndex() const { return m_selectedIdx; }
    const std::string& selectedPath() const { return m_selectedPath; }
    void select(int i) {
        if (i >= 0 && i < (int)m_files.size()) { m_selectedPath = m_files[i].fullPath; m_selectedIdx = i; }
        else clearSelection();
    }
    void clearSelection() { m_selectedPath.clear(); m_selectedIdx = -1; }
    const FileEntry* selected() const {
        return (m_selectedIdx >= 0 && m_selectedIdx < (int)m_files.size())
             ? &m_files[m_selectedIdx] : nullptr;
    }

    ViewMode viewMode() const { return m_viewMode; }
    void     toggleViewMode() {
        m_viewMode = (m_viewMode == ViewMode::Grid) ? ViewMode::List : ViewMode::Grid;
    }

    // ── Opening ─────────────────────────────────────────────────────────────
    // Double-click or "Load": a folder is entered here; a cookable file is
    // handed back to spawn; a text file is handed back to view. Entering a
    // folder only MARKS a rescan (it happens in the next update()), so the
    // list a front end is iterating stays valid for the rest of its frame.
    OpenRequest open(const FileEntry& f) {
        OpenRequest r;
        if (f.isDir)                    { navigate(f.fullPath); r.kind = OpenKind::Navigated; }
        else if (f.supported)           { r.kind = OpenKind::Spawn; r.entry = &f; }
        else if (isViewableText(f.ext)) { r.kind = OpenKind::ViewText; r.path = f.fullPath; }
        return r;
    }

    // ── The entry a context menu acts on ────────────────────────────────────
    // Captured when the menu opens, because the menu (and the modal it may
    // open) outlives the click that chose the entry.
    void setActionTarget(const FileEntry& f) {
        m_target          = f.fullPath;
        m_targetIsDir     = f.isDir;
        m_targetSupported = f.supported;
        m_targetExt       = f.ext;
    }
    const std::string& actionTarget() const { return m_target; }
    bool               actionTargetIsDir() const { return m_targetIsDir; }

    // "Open" from the context menu. The entry is looked up again by path,
    // since the list may have been rescanned while the menu was open.
    OpenRequest openActionTarget() {
        OpenRequest r;
        if (m_targetIsDir) { navigate(m_target); r.kind = OpenKind::Navigated; }
        else if (m_targetSupported) {
            for (auto& fe : m_files)
                if (fe.fullPath == m_target) { r.kind = OpenKind::Spawn; r.entry = &fe; break; }
        } else if (isViewableText(m_targetExt)) {
            r.kind = OpenKind::ViewText; r.path = m_target;
        }
        return r;
    }

    // ── Changing the project's files ────────────────────────────────────────
    // Each one rescans and tells the cooker, exactly as the panel always has.
    // Empty names are refused (the modals' "Create"/"Rename" did the same).
    bool createInCurrentDir(NewKind kind, const std::string& name, CookService* cook) {
        if (name.empty() || kind == NewKind::None) return false;
        const bool ok = (kind == NewKind::Folder)
                      ? createFolder(m_currentDir, name)
                      : !createScript(m_currentDir, name, kind).empty();
        requestRefresh(cook);
        return ok;
    }
    bool renameTarget(const std::string& newName, CookService* cook) {
        if (newName.empty()) return false;
        const bool ok = renamePath(m_target, newName);
        clearSelection();
        requestRefresh(cook);
        return ok;
    }
    bool deleteTarget(CookService* cook) {
        const bool ok = deletePath(m_target);
        clearSelection();
        requestRefresh(cook);
        return ok;
    }
    void duplicateTarget(CookService* cook) {
        duplicatePath(m_target);
        requestRefresh(cook);
    }

private:
    void resolveSelection() {
        m_selectedIdx = -1;
        if (m_selectedPath.empty()) return;
        for (int i = 0; i < (int)m_files.size(); ++i)
            if (m_files[i].fullPath == m_selectedPath) { m_selectedIdx = i; return; }
    }

    std::filesystem::path  m_root;
    std::filesystem::path  m_currentDir;
    std::vector<FileEntry> m_files;
    std::string            m_selectedPath;       // what is selected
    int                    m_selectedIdx = -1;   // where it is in m_files, or -1
    bool                   m_needRefresh = true;
    ViewMode               m_viewMode    = ViewMode::Grid;

    std::string m_target;
    bool        m_targetIsDir     = false;
    bool        m_targetSupported = false;
    std::string m_targetExt;
};

} // namespace ab
