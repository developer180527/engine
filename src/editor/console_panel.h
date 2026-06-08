#pragma once
#include <imgui.h>
#include "engine/logger.h"
#include "editor/terminal_panel.h"
#include "editor/editor_icons.h"

inline TerminalPanel& getTerminal() {
    static TerminalPanel t;
    return t;
}

inline void drawConsolePanel() {
    ImGui::Begin(ICON_FA_TERMINAL " Console");

    if (ImGui::BeginTabBar("##consoletabs")) {

        // ── Log tab ──────────────────────────────────────────────────
        if (ImGui::BeginTabItem("Log")) {
            if (ImGui::Button("Clear")) Logger::get().clear();
            ImGui::SameLine();
            static bool autoScroll = true;
            ImGui::Checkbox("Auto-scroll", &autoScroll);

            // Level filters
            ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
            static bool showLevels[5] = {true,true,true,true,true};
            const char*  lvlNames[]   = {"Debug","Info","OK","Warn","Error"};
            const ImVec4 lvlColors[]  = {
                {0.5f,0.5f,0.5f,1},{0.85f,0.85f,0.85f,1},
                {0.3f,0.9f,0.4f,1},{1.0f,0.8f,0.2f,1},{1.0f,0.35f,0.35f,1}
            };
            for (int i = 0; i < 5; ++i) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, lvlColors[i]);
                ImGui::Checkbox(lvlNames[i], &showLevels[i]);
                ImGui::PopStyleColor();
            }

            // Tag filter
            static char tagFilter[64] = {};
            ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputText("Tag", tagFilter, sizeof(tagFilter));

            ImGui::Separator();
            ImGui::BeginChild("##logentries", ImVec2(0,0), false,
                              ImGuiWindowFlags_HorizontalScrollbar);

            std::string tagStr = tagFilter;
            for (const auto& e : Logger::get().snapshot()) {
                int li = (int)e.level;
                if (!showLevels[li]) continue;
                if (!tagStr.empty() && e.tag.find(tagStr) == std::string::npos) continue;
                ImGui::TextDisabled("[%6.2f]", e.timestamp); ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, lvlColors[li]);
                ImGui::Text("[%s]", e.tag.c_str());
                ImGui::PopStyleColor(); ImGui::SameLine();
                ImGui::TextColored(lvlColors[li], "%s", e.message.c_str());
            }
            if (autoScroll) ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ── Terminal tab ─────────────────────────────────────────────
        if (ImGui::BeginTabItem("Terminal")) {
            getTerminal().draw();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::End();
}
