#pragma once
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "editor/editor_camera.h"
#include "core/logger.h"
#include "core/json_read.h"

// Lightweight editor state persistence.
// Saves/loads camera position and last selected entity name.
struct EditorPrefs {
    static void save(const std::filesystem::path& projectRoot,
                     const EditorCamera& cam,
                     const std::string&  selectedName) {
        nlohmann::json j;
        j["camera"]["position"] = {cam.position.x, cam.position.y, cam.position.z};
        j["camera"]["yaw"]      = cam.yaw;
        j["camera"]["pitch"]    = cam.pitch;
        j["selectedEntity"]     = selectedName;

        std::ofstream f(projectRoot / "editor.json");
        f << j.dump(2);
    }

    static void load(const std::filesystem::path& projectRoot,
                     EditorCamera& cam,
                     std::string&  outSelectedName) {
        auto p = projectRoot / "editor.json";
        if (!std::filesystem::exists(p)) return;
        try {
            nlohmann::json j = nlohmann::json::parse(std::ifstream(p));
            // Every read is bounds- and type-checked, and the try/catch above
            // is NOT what makes that safe: a short `position` array is
            // undefined behaviour, not an exception, so the handler never saw
            // it. `{"camera":{"position":[]}}` segfaulted here — on the
            // project-OPEN path, meaning a corrupt editor.json made the project
            // impossible to open. See core/json_read.h.
            if (j.is_object() && j.contains("camera")) {
                const auto& jc = j["camera"];
                jsonread::readFloats(jc, "position", &cam.position.x, 3);
                cam.yaw   = jsonread::readFloat(jc, "yaw",   cam.yaw);
                cam.pitch = jsonread::readFloat(jc, "pitch", cam.pitch);
            }
            if (j.is_object() && j.contains("selectedEntity")
                && j["selectedEntity"].is_string())
                outSelectedName = j["selectedEntity"].get<std::string>();
        } catch (...) {}
    }
};
