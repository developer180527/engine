#pragma once
#include <filesystem>
#include <string>
#include <vector>

// ── Project scaffolding (engine_core) ──────────────────────────────────────
// Creates new projects in the canonical v2 layout:
//
//   MyGame/
//   ├── project.json          name, version, engine, assetRoot, lastScene
//   ├── assets/               THE content root — single scan target
//   │   ├── scenes/main.scene
//   │   └── scripts/autorun/
//   ├── .cache/               generated (registry.db, cooked) — gitignored
//   └── .gitignore
//
// Used by the editor's project hub, the engine_project CLI, and SDK users
// who want to generate projects programmatically.
namespace project {

struct Template {
    std::string id;          // stable identifier ("empty", "basic3d")
    std::string name;        // display name
    std::string description; // one-liner for UI
};

// Available project templates, in display order.
const std::vector<Template>& templates();

// Create a project at `dir` (the project directory itself, e.g.
// ~/Projects/MyGame). Fails if dir exists and is non-empty. On failure
// returns false and fills `error`.
bool create(const std::filesystem::path& dir,
            const std::string& name,
            const std::string& templateId,
            std::string& error);

// True if `dir` looks like an openable project (has project.json).
inline bool isProject(const std::filesystem::path& dir) {
    return std::filesystem::exists(dir / "project.json");
}

} // namespace project
