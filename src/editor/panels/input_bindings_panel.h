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
#include "editor/panels/input_bindings/model.h"

inline void drawInputBindingsPanel(const std::filesystem::path& projectRoot,
                                   input::InputManager& mgr, bool* open) {
    if (open && !*open) return;
    static bindings::Document doc;
    static bool tried = false;

    if (!ImGui::Begin("Input Bindings", open)) { ImGui::End(); return; }

    if (!tried) { doc.load(projectRoot); tried = true; }
    if (!doc.loaded()) {
        ImGui::TextDisabled("no valid input.json in the project");
        if (ImGui::Button("Retry")) tried = false;
        ImGui::End(); return;
    }

    if (ImGui::Button("Save & Apply")) doc.saveAndApply(mgr);
    ImGui::SameLine();
    if (ImGui::Button("Revert")) doc.revert();
    if (doc.dirty()) { ImGui::SameLine(); ImGui::TextDisabled("(unsaved changes)"); }
    ImGui::Separator();

    // A capture in progress takes the next key the input system saw.
    if (doc.capturingAny()) {
        const int k = InputSystem::get().anyKeyPressedRaw();
        if (k >= 0) doc.acceptCapturedKey((Key)k);
    }

    for (size_t c = 0; c < doc.contextCount(); ++c) {
        const std::string cname = doc.contextName(c);
        if (!ImGui::CollapsingHeader(cname.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(cname.c_str());
        for (size_t a = 0; a < doc.actionCount(c); ++a) {
            ImGui::PushID((int)a);
            ImGui::Text("%-12s", doc.actionName(c, a).c_str());
            ImGui::SameLine(120);
            ImGui::TextDisabled("%s", doc.actionType(c, a).c_str());
            for (size_t b = 0; b < doc.bindingCount(c, a); ++b) {
                const bindings::Slot slot{c, a, b};
                ImGui::PushID((int)b);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "%s", doc.binding(slot).c_str());
                ImGui::SetNextItemWidth(180);
                ImGui::SameLine(200);
                if (ImGui::InputText("##spec", buf, sizeof(buf))) doc.setBinding(slot, buf);
                ImGui::SameLine();
                const bool capturing = doc.capturing(slot);
                if (ImGui::SmallButton(capturing ? "press a key…" : "cap")) {
                    if (capturing) doc.cancelCapture(); else doc.beginCapture(slot);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) { doc.removeBinding(slot); ImGui::PopID(); break; }
                ImGui::PopID();
                if (b + 1 < doc.bindingCount(c, a)) { ImGui::Text(""); }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ binding")) doc.addBinding(c, a);
            ImGui::PopID();
            ImGui::Separator();
        }
        ImGui::PopID();
    }
    ImGui::End();
}
