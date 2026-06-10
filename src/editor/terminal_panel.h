#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <array>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include "runtime/logger.h"

// Simple popen-based terminal rooted at the project directory.
// Not a full PTY — commands run, output is captured, shown in panel.
// Sufficient for git, cmake --build, ls, asset scripts, etc.
struct TerminalPanel {
    std::string              projectRoot;
    std::vector<std::string> history;     // output lines
    std::vector<std::string> cmdHistory;  // command history
    char                     inputBuf[512]{};
    int                      cmdHistoryIdx = -1;
    bool                     scrollToBottom = true;

    void setProjectRoot(const std::string& root) {
        projectRoot = root;
        history.clear();
        history.push_back("Terminal — " + root);
        history.push_back("Type a command and press Enter.");
        history.push_back("");
    }

    void runCommand(const std::string& cmd) {
        if (cmd.empty()) return;
        cmdHistory.push_back(cmd);
        cmdHistoryIdx = -1;
        history.push_back("$ " + cmd);

        // Run in project root
        std::string full = "cd " + projectRoot + " && " + cmd + " 2>&1";
        FILE* pipe = popen(full.c_str(), "r");
        if (!pipe) {
            history.push_back("[error] popen failed");
            return;
        }
        char buf[256];
        while (fgets(buf, sizeof(buf), pipe)) {
            std::string line = buf;
            // Strip trailing newline
            if (!line.empty() && line.back() == '\n') line.pop_back();
            history.push_back(line);
        }
        pclose(pipe);
        history.push_back("");
        scrollToBottom = true;
        LOG_DEBUG("Terminal", "Ran: %s", cmd.c_str());
    }

    void draw() {
        // Output area
        float footerHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
        ImGui::BeginChild("##termout",
                          ImVec2(0, -footerHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        for (const auto& line : history) {
            ImVec4 col = {0.85f, 0.85f, 0.85f, 1.0f};
            if (!line.empty() && line[0] == '$')
                col = {0.4f, 0.9f, 0.5f, 1.0f};  // command — green
            else if (line.find("[error]") != std::string::npos)
                col = {1.0f, 0.35f, 0.35f, 1.0f}; // error — red
            ImGui::TextColored(col, "%s", line.c_str());
        }

        if (scrollToBottom) {
            ImGui::SetScrollHereY(1.0f);
            scrollToBottom = false;
        }
        ImGui::EndChild();

        // Input bar
        ImGui::Separator();
        ImGui::SetNextItemWidth(-60.0f);

        ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue
                                  | ImGuiInputTextFlags_CallbackHistory;

        bool execute = ImGui::InputText("##cmd", inputBuf, sizeof(inputBuf),
            flags, [](ImGuiInputTextCallbackData* data) -> int {
                auto* tp = (TerminalPanel*)data->UserData;
                if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
                    int newIdx = tp->cmdHistoryIdx;
                    if (data->EventKey == ImGuiKey_UpArrow)
                        newIdx = std::min((int)tp->cmdHistory.size() - 1, newIdx + 1);
                    else if (data->EventKey == ImGuiKey_DownArrow)
                        newIdx = std::max(-1, newIdx - 1);
                    if (newIdx != tp->cmdHistoryIdx) {
                        tp->cmdHistoryIdx = newIdx;
                        std::string cmd = newIdx >= 0
                            ? tp->cmdHistory[tp->cmdHistory.size()-1-newIdx]
                            : "";
                        data->DeleteChars(0, data->BufTextLen);
                        data->InsertChars(0, cmd.c_str());
                    }
                }
                return 0;
            }, this);

        ImGui::SameLine();
        execute |= ImGui::Button("Run");

        if (execute && inputBuf[0] != '\0') {
            runCommand(inputBuf);
            std::memset(inputBuf, 0, sizeof(inputBuf));
            ImGui::SetKeyboardFocusHere(-1);
        }
    }
};
