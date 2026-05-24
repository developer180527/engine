#pragma once
// Asset browser panel — main entry point.
// Implementation is split across:
//   types.h    — FileEntry, RegistryInfo, ViewMode, formatters, icon styles
//   registry.h — queryRegistry, stateColor, stateName
//   spawn.h    — spawnFile, uniqueEntityName, autoScale, groundOffset
//   widgets.h  — scanDir, drawIconCell, drawFolderTree

#include "types.h"
#include "registry.h"
#include "spawn.h"
#include "widgets.h"

#include "engine_context.h"
#include "engine/async_loader.h"
#include "io/cook_service.h"
#include "components/spinner.h"
#include "components/mesh_renderer.h"

#include <imgui.h>
#include <flecs.h>
#include <filesystem>
#include <vector>

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

    // Reset when project changes
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
        // Live-update loaded flags every frame (cheap string-set lookup)
        for (auto& f : s_files)
            if (!f.isDir)
                f.loaded = ctx.importers.isLoaded(f.fullPath)
                         || loader.isLoaded(f.fullPath);
    }

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

            bool clicked = false;
            bool dbl = drawIconCell(i, f, s_selectedIdx == i, kIconW, kIconH, clicked);
            if (clicked)               s_selectedIdx = i;
            if (dbl && f.isDir)        { s_currentDir = f.fullPath; s_needRefresh = true; }
            if (dbl && !f.isDir && f.supported) spawnFile(f, ctx, loader);

            if (++col >= cols) { col = 0; ImGui::Dummy({0, 4}); }
        }
    } else {
        // List view
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
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (f.isDir)           { s_currentDir = f.fullPath; s_needRefresh = true; }
                    else if (f.supported)    spawnFile(f, ctx, loader);
                }
            }
            if (!f.supported && !f.isDir) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
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

        // Type icon preview
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
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", sel->reg.uuid.c_str());

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

        // Load & Spawn pinned to bottom of detail panel
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

    ImGui::End();
}
