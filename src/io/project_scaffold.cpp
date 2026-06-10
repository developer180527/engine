#include "io/project_scaffold.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace project {

const std::vector<Template>& templates() {
    static const std::vector<Template> kTemplates = {
        { "basic3d", "Basic 3D",
          "Camera, sun light and a cube — ready to press Play" },
        { "empty", "Empty",
          "Bare project: no entities, just the folder structure" },
    };
    return kTemplates;
}

// ── Template scenes ─────────────────────────────────────────────────────────
// Hand-built JSON in the SceneSerializer format (see io/entity_serializer.h).

static nlohmann::json makeTransform(std::initializer_list<float> pos,
                                    std::initializer_list<float> rot) {
    return {
        {"position", pos},
        {"rotation", rot},
        {"scale",    {1.0f, 1.0f, 1.0f}},
    };
}

static nlohmann::json basic3dScene() {
    nlohmann::json scene;
    scene["entities"] = nlohmann::json::array();

    // Primary camera at (0, 2, 8), identity rotation — looks down -Z at origin
    scene["entities"].push_back({
        {"id",   1},
        {"name", "MainCamera"},
        {"transform", makeTransform({0.0f, 2.0f, 8.0f}, {0, 0, 0, 1})},
        {"camera", {
            {"projection", 0}, {"fov", 60.0f},
            {"nearPlane", 0.1f}, {"farPlane", 1000.0f},
            {"orthoSize", 10.0f}, {"isPrimary", true},
            {"clearColor", {0.10f, 0.10f, 0.12f, 1.0f}},
        }},
    });

    // Directional sun pitched ~50 degrees down (quaternion about X)
    scene["entities"].push_back({
        {"id",   2},
        {"name", "Sun"},
        {"transform", makeTransform({0.0f, 10.0f, 0.0f},
                                    {-0.4226f, 0.0f, 0.0f, 0.9063f})},
        {"light", {
            {"type", 0}, {"color", {1.0f, 1.0f, 1.0f}},
            {"intensity", 3.0f}, {"range", 15.0f},
            {"spotInner", 25.0f}, {"spotOuter", 35.0f},
            {"castShadows", false},
            {"useTemperature", false}, {"temperatureK", 6500.0f},
        }},
    });

    // One cube to look at (engine primitive — no asset import needed)
    scene["entities"].push_back({
        {"id",   3},
        {"name", "Cube"},
        {"transform", makeTransform({0.0f, 0.5f, 0.0f}, {0, 0, 0, 1})},
        {"meshRenderer", {
            {"sourcePath", "engine://primitive/cube"},
            {"sourceType", "primitive"},
        }},
    });

    return scene;
}

static nlohmann::json emptyScene() {
    return {{"entities", nlohmann::json::array()}};
}

// ── Creation ────────────────────────────────────────────────────────────────

bool create(const std::filesystem::path& dir,
            const std::string& name,
            const std::string& templateId,
            std::string& error) {
    namespace fs = std::filesystem;

    if (name.empty())            { error = "project name is empty"; return false; }
    if (dir.empty())             { error = "project directory is empty"; return false; }
    if (fs::exists(dir) && !fs::is_directory(dir)) {
        error = "path exists and is not a directory: " + dir.string();
        return false;
    }
    if (fs::exists(dir) && !fs::is_empty(dir)) {
        error = "directory is not empty: " + dir.string();
        return false;
    }

    nlohmann::json scene;
    if      (templateId == "basic3d") scene = basic3dScene();
    else if (templateId == "empty")   scene = emptyScene();
    else { error = "unknown template: " + templateId; return false; }

    std::error_code ec;
    fs::create_directories(dir / "assets" / "scenes", ec);
    fs::create_directories(dir / "assets" / "scripts" / "autorun", ec);
    fs::create_directories(dir / ".cache", ec);
    if (ec) { error = "could not create directories: " + ec.message(); return false; }

    auto writeFile = [&](const fs::path& p, const std::string& text) {
        std::ofstream f(p);
        f << text;
        return f.good();
    };

    nlohmann::json pj = {
        {"version",   2},
        {"name",      name},
        {"engine",    "0.1.0"},
        {"assetRoot", "assets"},
        {"lastScene", "assets/scenes/main.scene"},
        {"template",  templateId},
    };

    if (!writeFile(dir / "project.json", pj.dump(2) + "\n") ||
        !writeFile(dir / "assets" / "scenes" / "main.scene", scene.dump(2) + "\n") ||
        !writeFile(dir / ".gitignore", ".cache/\n")) {
        error = "failed writing project files in " + dir.string();
        return false;
    }
    return true;
}

} // namespace project
