#pragma once
// Script viewer — the ImGui front end of code::DocSet (script_viewer/model.h):
// one floating window per open document, drawing the model's coloured spans.
#include <imgui.h>
#include "editor/panels/script_viewer/model.h"

class ScriptViewer {
public:
    void open(const std::string& path) { m_set.open(path); }
    bool anyOpen() const { return m_set.anyOpen(); }

    void draw() {
        const std::string focus = m_set.takeFocus();
        for (auto& d : m_set.docs()) {
            if (!d.open) continue;
            if (d.path == focus) ImGui::SetNextWindowFocus();
            ImGui::SetNextWindowSize(ImVec2(680, 520), ImGuiCond_FirstUseEver);
            std::string id = d.title + "###scriptview_" + d.path;
            if (ImGui::Begin(id.c_str(), &d.open)) {
                ImGui::TextDisabled("%s", code::langName(d.lang));
                ImGui::SameLine(); ImGui::TextDisabled("  %s", d.path.c_str());
                if (d.truncated) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.9f,0.7f,0.3f,1), " (truncated)");
                }
                ImGui::Separator();
                drawCode(d);
            }
            ImGui::End();
        }
        m_set.prune();
    }

private:
    static void drawCode(const code::Doc& d) {
        ImGui::BeginChild("##code", ImVec2(0,0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 1));
        int ln = 1;
        for (const auto& line : d.lines) {
            ImGui::TextDisabled("%4d", ln++);
            ImGui::SameLine(0, 14);
            if (line.empty()) { ImGui::TextUnformatted(" "); continue; }
            for (size_t s = 0; s < line.size(); ++s) {
                if (s) ImGui::SameLine(0, 0);
                const edui::Color c = line[s].col;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(c.r, c.g, c.b, c.a));
                ImGui::TextUnformatted(line[s].text.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }

    code::DocSet m_set;
};
