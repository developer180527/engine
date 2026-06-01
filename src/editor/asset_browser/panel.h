#pragma once
// Asset browser panel — main entry point.
// Split across: types.h registry.h spawn.h widgets.h actions.h
// Owns a ScriptViewer (function-static) for double-click code viewing; its
// floating windows are drawn after the Assets window's End() so they're real
// top-level windows. Fully self-contained — no editor_app changes required.

#include "types.h"
#include "registry.h"
#include "spawn.h"
#include "widgets.h"
#include "actions.h"

#include "engine_context.h"
#include "engine/async_loader.h"
#include "io/cook_service.h"
#include "components/spinner.h"
#include "components/mesh_renderer.h"
#include "editor/script_viewer.h"

#include <imgui.h>
#include <flecs.h>
#include <filesystem>
#include <vector>
#include <cstring>

inline void drawAssetBrowserPanel(EngineContext& ctx, AsyncLoader& loader,
                                  CookService* cookService = nullptr) {
    using namespace ab;
    namespace fs = std::filesystem;

    static fs::path           s_root;
    static fs::path           s_currentDir;
    static std::vector<FileEntry> s_files;
    static int                s_selectedIdx = -1;
    static bool               s_needRefresh = true;
    static ViewMode           s_viewMode    = ViewMode::Grid;
    static ScriptViewer       s_scriptViewer;

    // context-menu / modal state
    static ab::NewKind s_newKind = ab::NewKind::None;
    static char        s_nameBuf[128] = {};
    static bool        s_openCreate = false, s_openRename = false, s_openDelete = false;
    static std::string s_actionTarget;
    static bool        s_actionIsDir = false, s_actionSupported = false;
    static std::string s_actionExt;

    if (s_root.empty() || s_root != ctx.project.assetsRoot) {
        s_root       = ctx.project.assetsRoot;
        s_currentDir = s_root;
        s_needRefresh = true;
    }
    auto cacheRoot = ctx.project.projectRoot / ".cache";

    ImGui::Begin("Assets");

    // ── Toolbar ──────────────────────────────────────────────────────────────
    {
        std::error_code ec;
        auto rel = fs::relative(s_currentDir, s_root, ec);
        std::string crumb = "assets";
        if (!ec && rel != ".") crumb += " / " + rel.generic_string();
        ImGui::TextDisabled("%s", crumb.c_str());
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110);
    if (ImGui::Button("Refresh")) {
        s_needRefresh = true;
        s_selectedIdx = -1;
        if (cookService) cookService->requestRefresh();
    }
    ImGui::SameLine();
    if (ImGui::Button(s_viewMode == ViewMode::Grid ? "[=]" : "[#]", {36,0}))
        s_viewMode = (s_viewMode == ViewMode::Grid) ? ViewMode::List : ViewMode::Grid;

    ImGui::Separator();

    // ── File list refresh ─────────────────────────────────────────────────────
    if (s_needRefresh) {
        s_files = scanDir(s_currentDir, ctx.importers,
                          ctx.assetLib, ctx.project.projectRoot, cacheRoot);
        s_needRefresh = false;
    } else {
        for (auto& f : s_files)
            if (!f.isDir)
                f.loaded = ctx.importers.isLoaded(f.fullPath)
                         || loader.isLoaded(f.fullPath);
    }

    // shared routing helpers
    auto setAction = [&](const FileEntry& f) {
        s_actionTarget    = f.fullPath;
        s_actionIsDir     = f.isDir;
        s_actionSupported = f.supported;
        s_actionExt       = f.ext;
    };
    auto openEntry = [&](const FileEntry& f) {
        if (f.isDir)            { s_currentDir = f.fullPath; s_needRefresh = true; }
        else if (f.supported)     spawnFile(f, ctx, loader);
        else if (isViewableText(f.ext)) s_scriptViewer.open(f.fullPath);
    };

    // ── 3-column layout: [tree] [grid] [detail] ──────────────────────────────
    constexpr float kTreeW   = 165.f;
    constexpr float kDetailW = 225.f;
    float           fullH    = ImGui::GetContentRegionAvail().y
                               - ImGui::GetFrameHeightWithSpacing() - 4;

    // ── Left: folder tree ────────────────────────────────────────────────────
    ImGui::BeginChild("##tree", {kTreeW, fullH}, true);
    drawFolderTree(s_root, s_root, s_currentDir, s_needRefresh);
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Center: file grid / list ─────────────────────────────────────────────
    ImGui::BeginChild("##grid", {-(kDetailW + 6), fullH}, false);

    if (s_files.empty()) {
        ImGui::TextDisabled("(empty)");
    } else if (s_viewMode == ViewMode::Grid) {
        constexpr float kIconW = 74.f;
        constexpr float kIconH = 52.f;
        constexpr float kCellW = kIconW + 10.f;
        int cols = std::max(1, (int)(ImGui::GetContentRegionAvail().x / kCellW));
        int col  = 0;

        for (int i = 0; i < (int)s_files.size(); ++i) {
            auto& f = s_files[i];
            if (col > 0) ImGui::SameLine(0, 4);

            bool clicked = false, rclick = false;
            bool dbl = drawIconCell(i, f, s_selectedIdx == i, kIconW, kIconH,
                                    clicked, rclick);
            if (clicked) s_selectedIdx = i;
            if (rclick)  { s_selectedIdx = i; setAction(f); ImGui::OpenPopup("##itemctx"); }
            if (dbl)     openEntry(f);

            if (++col >= cols) { col = 0; ImGui::Dummy({0, 4}); }
        }
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 2});
        for (int i = 0; i < (int)s_files.size(); ++i) {
            auto& f = s_files[i];
            ImGui::PushID(i);

            if (!f.isDir && f.reg.found) {
                ImVec2 p = ImGui::GetCursorScreenPos();
                p.y += ImGui::GetTextLineHeight() * 0.5f;
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    {p.x+6, p.y}, 4.f, stateColor(f.reg.state));
            }
            ImGui::Dummy({14,0}); ImGui::SameLine();

            std::string row = (f.isDir ? "[DIR] " : "      ") + f.name;
            if (!f.isDir) row += "  " + formatSize(f.sizeBytes);

            if (!f.supported && !f.isDir)
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

            if (ImGui::Selectable(row.c_str(), s_selectedIdx == i,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                s_selectedIdx = i;
                if (ImGui::IsMouseDoubleClicked(0)) openEntry(f);
            }
            if (!f.supported && !f.isDir) ImGui::PopStyleColor();

            if (!f.isDir && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("ASSET_PATH", f.fullPath.c_str(),
                                          f.fullPath.size() + 1);
                ImGui::TextUnformatted(f.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                s_selectedIdx = i; setAction(f); ImGui::OpenPopup("##itemctx");
            }
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }

    // ── Per-item context menu (operates on captured s_actionTarget) ───────────
    if (ImGui::BeginPopup("##itemctx")) {
        fs::path tp = s_actionTarget;
        ImGui::TextDisabled("%s", tp.filename().string().c_str());
        ImGui::Separator();
        if (ImGui::MenuItem("Open")) {
            if (s_actionIsDir) { s_currentDir = tp; s_needRefresh = true; }
            else if (s_actionSupported) {
                for (auto& fe : s_files)
                    if (fe.fullPath == s_actionTarget) { spawnFile(fe, ctx, loader); break; }
            } else if (isViewableText(s_actionExt)) {
                s_scriptViewer.open(s_actionTarget);
            }
        }
        if (ImGui::MenuItem("Rename")) {
            std::strncpy(s_nameBuf, tp.filename().string().c_str(), sizeof s_nameBuf - 1);
            s_nameBuf[sizeof s_nameBuf - 1] = 0;
            s_openRename = true;
        }
        if (!s_actionIsDir && ImGui::MenuItem("Duplicate")) {
            ab::duplicatePath(tp);
            s_needRefresh = true; if (cookService) cookService->requestRefresh();
        }
        if (ImGui::MenuItem("Copy Path")) ImGui::SetClipboardText(s_actionTarget.c_str());
        if (ImGui::MenuItem("Reveal in Finder")) ab::revealInFinder(tp);
        ImGui::Separator();
        if (ImGui::MenuItem("Delete")) s_openDelete = true;
        ImGui::EndPopup();
    }

    // ── Empty-area context menu — manual open, gated on "no item hovered" so
    //    it can never collide with the per-item menu on the same right-click ──
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered()
        && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        ImGui::OpenPopup("##bgctx");
    if (ImGui::BeginPopup("##bgctx")) {
        if (ImGui::MenuItem("New Folder")) {
            s_newKind = ab::NewKind::Folder;
            std::strncpy(s_nameBuf, "New Folder", sizeof s_nameBuf - 1);
            s_nameBuf[sizeof s_nameBuf - 1] = 0; s_openCreate = true;
        }
        if (ImGui::BeginMenu("New Script")) {
            auto pick = [&](ab::NewKind k){
                s_newKind = k;
                std::strncpy(s_nameBuf, "NewScript", sizeof s_nameBuf - 1);
                s_nameBuf[sizeof s_nameBuf - 1] = 0; s_openCreate = true;
            };
            if (ImGui::MenuItem("Lua"))    pick(ab::NewKind::ScriptLua);
            if (ImGui::MenuItem("Python")) pick(ab::NewKind::ScriptPython);
            if (ImGui::MenuItem("C++"))    pick(ab::NewKind::ScriptCpp);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reveal in Finder")) ab::revealInFinder(s_currentDir);
        if (ImGui::MenuItem("Refresh")) {
            s_needRefresh = true; if (cookService) cookService->requestRefresh();
        }
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ── Right: detail panel ──────────────────────────────────────────────────
    ImGui::BeginChild("##detail", {kDetailW, fullH}, true);

    const FileEntry* sel = (s_selectedIdx >= 0 && s_selectedIdx < (int)s_files.size())
                         ? &s_files[s_selectedIdx] : nullptr;

    if (sel && !sel->isDir) {
        ImGui::TextColored({1, 0.9f, 0.5f, 1}, "%s", sel->name.c_str());
        ImGui::Separator();
        {
            auto sty = iconStyle(sel->ext);
            ImVec2 p  = ImGui::GetCursorScreenPos();
            float  sz = kDetailW - 24.f;
            float  ih = sz * 0.52f;
            ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x+sz, p.y+ih}, sty.bg, 8.f);
            auto ts = ImGui::CalcTextSize(sty.label);
            float scale = 1.8f;
            ImGui::GetWindowDrawList()->AddText(
                ImGui::GetFont(), ImGui::GetFontSize() * scale,
                {p.x+(sz-ts.x*scale)*.5f, p.y+(ih-ts.y*scale)*.5f},
                sty.fg, sty.label);
            ImGui::Dummy({sz, ih});
            ImGui::Spacing();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {4, 4});

        ImGui::TextDisabled("Size    "); ImGui::SameLine(62);
        ImGui::Text("%s", formatSize(sel->sizeBytes).c_str());

        if (sel->reg.found) {
            ImGui::Separator();
            ImGui::TextDisabled("UUID    "); ImGui::SameLine(62);
            std::string sid = sel->reg.uuid.substr(0,14) + "...";
            ImGui::TextUnformatted(sid.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", sel->reg.uuid.c_str());

            ImGui::TextDisabled("State   "); ImGui::SameLine(62);
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(stateColor(sel->reg.state)),
                "* %s", stateName(sel->reg.state));

            ImGui::TextDisabled("Cook v  "); ImGui::SameLine(62);
            ImGui::Text("%u", sel->reg.cookVersion);

            if (sel->reg.cookedBytes > 0) {
                ImGui::TextDisabled("Cooked  "); ImGui::SameLine(62);
                ImGui::Text("%s", formatSize(sel->reg.cookedBytes).c_str());
                if (sel->sizeBytes > 0) {
                    float ratio = (float)sel->reg.cookedBytes / sel->sizeBytes * 100.f;
                    ImGui::TextDisabled("Ratio   "); ImGui::SameLine(62);
                    ImGui::TextDisabled("%.0f%% of source", ratio);
                }
            }
            if (sel->reg.cookedAt > 0) {
                ImGui::Separator();
                ImGui::TextDisabled("Cooked at");
                ImGui::TextDisabled("%s", formatTime(sel->reg.cookedAt).c_str());
            }
        } else {
            ImGui::Separator();
            ImGui::TextDisabled(sel->supported ? "Not in registry" : "Not cookable");
        }

        ImGui::PopStyleVar();

        ImGui::SetCursorPosY(ImGui::GetWindowHeight()
                             - ImGui::GetFrameHeightWithSpacing() - 6);
        ImGui::Separator();
        const bool canLoad = sel->supported;
        if (canLoad && loader.isLoading(sel->fullPath))
            ImGui::TextDisabled("Loading...");
        else {
            if (!canLoad) ImGui::BeginDisabled();
            if (ImGui::Button("Load & Spawn", {-1, 0}) && canLoad)
                spawnFile(*sel, ctx, loader);
            if (!canLoad) ImGui::EndDisabled();
        }

    } else if (sel && sel->isDir) {
        ImGui::TextColored({1, 0.85f, 0.4f, 1}, "[DIR] %s", sel->name.c_str());
        ImGui::Separator();
        ImGui::TextDisabled("Double-click to open");
    } else {
        ImGui::TextDisabled("Select a file\nto see details");
    }

    ImGui::EndChild();

    // ── Bottom bar ────────────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::Button("Clear Scene", {120, 0})) {
        std::vector<flecs::entity> toDelete;
        ctx.ecs.query_builder<MeshRenderer>().build()
            .each([&](flecs::entity e, MeshRenderer&) {
                if (!e.has<Spinner>()) toDelete.push_back(e);
            });
        for (auto e : toDelete) e.destruct();
        ctx.editor.selected = flecs::entity{};
    }

    // ── Open modals from flags (window scope = matches modal scope) ───────────
    if (s_openCreate) { ImGui::OpenPopup("Create New###createmodal"); s_openCreate = false; }
    if (s_openRename) { ImGui::OpenPopup("Rename###renamemodal");     s_openRename = false; }
    if (s_openDelete) { ImGui::OpenPopup("Delete?###deletemodal");    s_openDelete = false; }

    if (ImGui::BeginPopupModal("Create New###createmodal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(s_newKind == ab::NewKind::Folder ? "Folder name"
                                                                : "Script name");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##cn", s_nameBuf, sizeof s_nameBuf,
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        bool ok = ImGui::Button("Create", {120,0}) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120,0})) { s_newKind = ab::NewKind::None; ImGui::CloseCurrentPopup(); }
        if (ok && s_nameBuf[0]) {
            if (s_newKind == ab::NewKind::Folder) {
                ab::createFolder(s_currentDir, s_nameBuf);
            } else {
                ab::createScript(s_currentDir, s_nameBuf, s_newKind);
            }
            s_needRefresh = true; if (cookService) cookService->requestRefresh();
            s_newKind = ab::NewKind::None; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Rename###renamemodal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("New name");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputText("##rn", s_nameBuf, sizeof s_nameBuf,
                                      ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Spacing();
        bool ok = ImGui::Button("Rename", {120,0}) || enter;
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120,0})) ImGui::CloseCurrentPopup();
        if (ok && s_nameBuf[0]) {
            ab::renamePath(s_actionTarget, s_nameBuf);
            s_needRefresh = true; s_selectedIdx = -1;
            if (cookService) cookService->requestRefresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete?###deletemodal", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete \"%s\"?",
                    fs::path(s_actionTarget).filename().string().c_str());
        ImGui::TextDisabled("This cannot be undone.");
        ImGui::Spacing();
        if (ImGui::Button("Delete", {120,0})) {
            ab::deletePath(s_actionTarget);
            s_needRefresh = true; s_selectedIdx = -1;
            if (cookService) cookService->requestRefresh();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {120,0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::End();

    // Floating code-viewer windows (top-level, after Assets' End()).
    s_scriptViewer.draw();
}
