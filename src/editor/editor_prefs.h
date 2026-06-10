#pragma once
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include "editor/editor_camera.h"
#include "core/logger.h"

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
            if (j.contains("camera")) {
                const auto& jc = j["camera"];
                if (jc.contains("position"))
                    cam.position = {jc["position"][0],
                                    jc["position"][1],
                                    jc["position"][2]};
                cam.yaw   = jc.value("yaw",   0.0f);
                cam.pitch = jc.value("pitch",  0.0f);
            }
            outSelectedName = j.value("selectedEntity", "");
        } catch (...) {}
    }
};
