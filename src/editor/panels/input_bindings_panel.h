#pragma once
// ── Input Bindings panel — edit the project's input.json live ───────────────
// The rebinding UI (backlog C#14). Edits the FILE (the developer-owned
// source of truth), never the manager's compiled state: Save & Apply writes
// input.json and hot-reloads the InputManager, so bindings change mid-play.
// Capture flow: click [cap], press a key — the reverse keymap turns the raw
// code into a "key:Name" spec. TODO(#13 gamepad): capture pad buttons/axes
// here once 'pad:' specs exist.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <imgui.h>
#include <json.hpp>

#include "runtime/input/input_manager.h"
#include "runtime/input/input_system.h"
#include "runtime/input/hid_keymap.h"
#include "core/logger.h"

inline void drawInputBindingsPanel(const std::filesystem::path& projectRoot,
                                   input::InputManager& mgr, bool* open) {
    if (open && !*open) return;
    static nlohmann::json j;
    static bool loaded = false, dirty = false;
    static int  capturing = -1;              // row id waiting for a key
    const auto path = projectRoot / "input.json";

    if (!ImGui::Begin("Input Bindings", open)) { ImGui::End(); return; }

    auto loadFile = [&] {
        std::ifstream f(path);
        std::stringstream ss; ss << f.rdbuf();
        j = nlohmann::json::parse(ss.str(), nullptr, false);
        loaded = !j.is_discarded() && j.contains("contexts");
        dirty = false; capturing = -1;
    };
    if (!loaded) loadFile();
    if (!loaded) {
        ImGui::TextDisabled("no valid input.json in the project");
        if (ImGui::Button("Retry")) loaded = false;
        ImGui::End(); return;
    }

    // ── Toolbar ─────────────────────────────────────────────────────────────
    if (ImGui::Button("Save & Apply")) {
        std::ofstream out(path);
        out << j.dump(2);
        out.close();
        mgr.loadProjectBindings(projectRoot);   // hot-reload, mid-play too
        dirty = false;
        LOG_SUCCESS("Input", "bindings saved + applied");
    }
    ImGui::SameLine();
    if (ImGui::Button("Revert")) loadFile();
    if (dirty) { ImGui::SameLine(); ImGui::TextDisabled("(unsaved changes)"); }
    ImGui::Separator();

    // ── Contexts -> actions -> binding specs ────────────────────────────────
    int row = 0;
    for (auto& jc : j["contexts"]) {
        const std::string cname = jc.value("name", "unnamed");
        if (!ImGui::CollapsingHeader(cname.c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(cname.c_str());
        for (auto& ja : jc["actions"]) {
            ImGui::PushID(ja.value("name", "?").c_str());
            ImGui::Text("%-12s", ja.value("name", "?").c_str());
            ImGui::SameLine(120);
            ImGui::TextDisabled("%s", ja.value("type", "digital").c_str());
            auto& binds = ja["bindings"];
            for (size_t b = 0; b < binds.size(); ++b, ++row) {
                ImGui::PushID((int)b);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s",
                              binds[b].get<std::string>().c_str());
                ImGui::SetNextItemWidth(180);
                ImGui::SameLine(200);
                if (ImGui::InputText("##spec", buf, sizeof(buf)))
                    { binds[b] = std::string(buf); dirty = true; }
                ImGui::SameLine();
                // Capture: next raw key press becomes "key:Name".
                const bool isCapturing = (capturing == row);
                if (ImGui::SmallButton(isCapturing ? "press a key…" : "cap"))
                    capturing = isCapturing ? -1 : row;
                if (capturing == row) {
                    const int k = InputSystem::get().anyKeyPressedRaw();
                    if (const char* n = k >= 0 ? input::nameFromGlfw(k) : nullptr) {
                        binds[b] = std::string("key:") + n;
                        dirty = true; capturing = -1;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    binds.erase(binds.begin() + (long)b);
                    dirty = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
                if (b + 1 < binds.size()) { ImGui::Text(""); }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ binding")) {
                binds.push_back("key:F1");
                dirty = true;
            }
            ImGui::PopID();
            ImGui::Separator();
        }
        ImGui::PopID();
    }
    ImGui::End();
}
