#pragma once
#include "types.h"
#include "registry.h"
#include "scan.h"
#include "model.h"
#include <imgui.h>
#include <filesystem>
#include <vector>
#include <algorithm>

namespace ab {

// The one place a browser colour becomes an ImGui colour.
inline ImU32 toImU32(Rgba c) { return IM_COL32(c.r, c.g, c.b, c.a); }


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
    dl->AddRectFilled(ip, {ip.x+isz.x, ip.y+isz.y}, toImU32(sty.bg), 6.f);

    // Type label centered
    auto ts = ImGui::CalcTextSize(sty.label);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                {ip.x+(isz.x-ts.x)*.5f, ip.y+(isz.y-ts.y)*.5f-3},
                toImU32(sty.fg), sty.label);

    // Loaded marker (small green square, top-left)
    if (f.loaded)
        dl->AddRectFilled(ip, {ip.x+12, ip.y+12}, IM_COL32(60,200,80,210), 3.f);

    // Registry state badge (circle, bottom-right)
    if (f.reg.found && !f.isDir) {
        ImVec2 bc = {ip.x+isz.x-8, ip.y+isz.y-8};
        dl->AddCircleFilled(bc, 7.f, IM_COL32(18,18,18,220));
        dl->AddCircleFilled(bc, 5.f, toImU32(stateColor(f.reg.state)));
    }

    // Filename label (truncated, centered below icon)
    std::string lbl = f.name.size()>13 ? f.name.substr(0,11)+".." : f.name;
    auto ls = ImGui::CalcTextSize(lbl.c_str());
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                {cur.x+(cellW-ls.x)*.5f, cur.y+iconH+4},
                IM_COL32(210,210,210,255), lbl.c_str());
    return dbl;
}

// Draw the recursive folder tree; a click navigates the model there.
inline void drawFolderTree(const std::filesystem::path& dir, AssetBrowserModel& model) {
    const std::vector<std::filesystem::path> subs = listSubdirs(dir);
    const bool  hasSubs  = !subs.empty();
    const bool  isRoot   = (dir == model.root());
    std::string label    = isRoot ? "assets" : dir.filename().string();
    bool        selected = (dir == model.currentDir());

    ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_OpenOnArrow |
        ImGuiTreeNodeFlags_SpanFullWidth;
    if (selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (!hasSubs) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (isRoot)   flags |= ImGuiTreeNodeFlags_DefaultOpen;

    bool open = ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        model.navigate(dir);

    if (open && hasSubs) {
        for (auto& s : subs) drawFolderTree(s, model);
        ImGui::TreePop();
    }
}

} // namespace ab
