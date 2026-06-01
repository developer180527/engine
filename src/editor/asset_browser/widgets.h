#pragma once
#include "types.h"
#include "registry.h"
#include "io/importer_registry.h"
#include <imgui.h>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace ab {

// Scan one directory level and return sorted FileEntry list.
inline std::vector<FileEntry> scanDir(const std::filesystem::path& dir,
                                      const ImporterRegistry&      importers,
                                      assetlib::AssetRegistry*     reg,
                                      const std::filesystem::path& projectRoot,
                                      const std::filesystem::path& cacheRoot) {
    std::vector<FileEntry> out;
    if (!std::filesystem::is_directory(dir)) return out;
    for (const auto& de : std::filesystem::directory_iterator(dir)) {
        FileEntry e;
        e.name     = de.path().filename().string();
        e.fullPath = de.path().string();
        e.isDir    = de.is_directory();
        if (!e.isDir) {
            if (!de.is_regular_file()) continue;
            e.ext       = lowerExt(de.path());
            e.supported = importers.supportsFile(de.path());
            try { e.sizeBytes = de.file_size(); } catch (...) {}
            e.reg = queryRegistry(reg, e.fullPath, projectRoot, cacheRoot);
        }
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b){
        if (a.isDir    != b.isDir)    return a.isDir    > b.isDir;
        if (a.supported!= b.supported)return a.supported> b.supported;
        return a.name < b.name;
    });
    return out;
}

// Draw a single icon cell in grid view.
// Returns true on double-click; sets singleClick=true on single click.
inline bool drawIconCell(int id, const FileEntry& f, bool selected,
                         float cellW, float iconH, bool& singleClick, bool& rightClick) {
    singleClick = false;
    rightClick  = false;
    auto* dl   = ImGui::GetWindowDrawList();
    ImVec2 cur = ImGui::GetCursorScreenPos();

    ImGui::PushID(id);
    bool dbl = false;
    ImGui::InvisibleButton("##ic", {cellW, iconH + 20});
    if (ImGui::IsItemClicked())                                  singleClick = true;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right))            rightClick  = true;
    if (ImGui::IsMouseDoubleClicked(0) && ImGui::IsItemHovered()) dbl = true;
    if (!f.isDir && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("ASSET_PATH", f.fullPath.c_str(),
                                  f.fullPath.size() + 1);
        ImGui::TextUnformatted(f.name.c_str());
        ImGui::EndDragDropSource();
    }
    ImGui::PopID();

    // Hover / selection highlight
    if (selected || ImGui::IsItemHovered()) {
        ImU32 col = selected ? IM_COL32(80,140,220,70) : IM_COL32(255,255,255,18);
        dl->AddRectFilled(cur, {cur.x+cellW, cur.y+iconH+20}, col, 5.f);
    }

    float   pad = 8.f;
    ImVec2  ip  = {cur.x+pad,      cur.y+4};
    ImVec2  isz = {cellW-pad*2,    iconH-8};
    auto    sty = iconStyle(f.ext, f.isDir);

    // Icon body
    dl->AddRectFilled(ip, {ip.x+isz.x, ip.y+isz.y}, sty.bg, 6.f);

    // Type label centered
    auto ts = ImGui::CalcTextSize(sty.label);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                {ip.x+(isz.x-ts.x)*.5f, ip.y+(isz.y-ts.y)*.5f-3},
                sty.fg, sty.label);

    // Loaded marker (small green square, top-left)
    if (f.loaded)
        dl->AddRectFilled(ip, {ip.x+12, ip.y+12}, IM_COL32(60,200,80,210), 3.f);

    // Registry state badge (circle, bottom-right)
    if (f.reg.found && !f.isDir) {
        ImVec2 bc = {ip.x+isz.x-8, ip.y+isz.y-8};
        dl->AddCircleFilled(bc, 7.f, IM_COL32(18,18,18,220));
        dl->AddCircleFilled(bc, 5.f, stateColor(f.reg.state));
    }

    // Filename label (truncated, centered below icon)
    std::string lbl = f.name.size()>13 ? f.name.substr(0,11)+".." : f.name;
    auto ls = ImGui::CalcTextSize(lbl.c_str());
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                {cur.x+(cellW-ls.x)*.5f, cur.y+iconH+4},
                IM_COL32(210,210,210,255), lbl.c_str());
    return dbl;
}

// Draw recursive folder tree; sets currentDir and needRefresh on click.
inline void drawFolderTree(const std::filesystem::path& dir,
                           const std::filesystem::path& root,
                           std::filesystem::path&       currentDir,
                           bool&                        needRefresh) {
    namespace fs = std::filesystem;
    bool hasSubs = false;
    try { for (const auto& e : fs::directory_iterator(dir))
              if (e.is_directory()) { hasSubs = true; break; }
    } catch (...) {}

    std::string label     = (dir == root) ? "assets" : dir.filename().string();
    bool        selected  = (dir == currentDir);

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanFullWidth;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (!hasSubs) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (dir==root)flags |= ImGuiTreeNodeFlags_DefaultOpen;

    bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        currentDir  = dir;
        needRefresh = true;
    }

    if (open && hasSubs) {
        try {
            std::vector<fs::path> subs;
            for (const auto& e : fs::directory_iterator(dir))
                if (e.is_directory()) subs.push_back(e.path());
            std::sort(subs.begin(), subs.end());
            for (auto& s : subs)
                drawFolderTree(s, root, currentDir, needRefresh);
        } catch (...) {}
        ImGui::TreePop();
    }
}

} // namespace ab
