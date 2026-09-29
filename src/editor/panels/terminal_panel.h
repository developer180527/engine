#pragma once
#include <imgui.h>
#include <string>
#include <vector>
#include <array>
#include <filesystem>
#include <cstdio>
#include <cstring>

// ── popen/pclose, which Windows spells with an underscore ────────────────────
// POSIX has popen/pclose in <stdio.h>; MSVC has _popen/_pclose in the same
// header and does NOT provide the unprefixed names, so this panel was
// "error C3861: 'popen': identifier not found" on every Windows build. Aliased
// here rather than ifdef'd at the two call sites: the panel wants "run a command
// and read its output", which is one capability with two spellings.
//
// The shell differs too — cmd.exe takes `&&` and `cd` the same way for this
// purpose, so the command string itself needs no change.
#include "editor/panels/terminal/model.h"
#include <algorithm>

// The ImGui front end of term::TerminalSession: the session (running,
// output, history) is the model's; this adds the input line.
struct TerminalPanel : term::TerminalSession {
    char inputBuf[512]{};

    void draw() {
        // Output area
        float footerHeight = ImGui::GetFrameHeightWithSpacing() + 4.0f;
        ImGui::BeginChild("##termout",
                          ImVec2(0, -footerHeight), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        for (const auto& line : history) {
            const edui::Color c = lineColor(line);
            ImGui::TextColored(ImVec4(c.r, c.g, c.b, c.a), "%s", line.c_str());
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
                    bool changed = false;
                    const std::string cmd = tp->stepHistory(
                        data->EventKey == ImGuiKey_UpArrow ? +1 : -1, changed);
                    if (changed) {
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
