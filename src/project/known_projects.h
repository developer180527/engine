#pragma once
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "project/project_context.h"

// ── Known projects list (engine_core) ──────────────────────────────────────
// The user-level registry behind the editor's project hub and
// `engine_project list`: ~/.engine/projects.json. Pure file I/O — no engine
// state. The legacy ~/.engine/last_project.txt is merged in on load so
// existing projects show up in the hub the first time it opens.
namespace project {

struct KnownProject {
    std::string           name;
    std::filesystem::path path;
    long long             lastOpened = 0; // unix seconds, 0 = never

    bool exists() const {
        return std::filesystem::exists(path / "project.json");
    }
};

class KnownProjects {
public:
    static std::filesystem::path file() {
        auto home = std::filesystem::path(
            getenv("HOME") ? getenv("HOME") : ".");
        auto dir = home / ".engine";
        std::filesystem::create_directories(dir);
        return dir / "projects.json";
    }

    static std::vector<KnownProject> load() {
        std::vector<KnownProject> out;
        auto p = file();
        if (std::filesystem::exists(p)) {
            try {
                std::ifstream f(p);
                nlohmann::json j = nlohmann::json::parse(f);
                for (auto& e : j.value("projects", nlohmann::json::array())) {
                    KnownProject kp;
                    kp.name       = e.value("name", "");
                    kp.path       = std::filesystem::path(e.value("path", ""));
                    kp.lastOpened = e.value("lastOpened", 0LL);
                    if (!kp.path.empty()) out.push_back(std::move(kp));
                }
            } catch (...) { /* corrupt list — start fresh */ }
        }

        // One-time migration: surface the legacy "last project" entry.
        auto legacy = ProjectContext::loadLastProjectPath();
        if (!legacy.empty() && std::filesystem::exists(legacy / "project.json")) {
            bool known = std::any_of(out.begin(), out.end(),
                [&](const KnownProject& kp) { return kp.path == legacy; });
            if (!known) {
                auto ctx = ProjectContext::load(legacy);
                out.push_back({ctx.name, legacy, 0});
            }
        }

        // Most recently opened first.
        std::sort(out.begin(), out.end(),
            [](const KnownProject& a, const KnownProject& b) {
                return a.lastOpened > b.lastOpened;
            });
        return out;
    }

    static void save(const std::vector<KnownProject>& projects) {
        nlohmann::json j;
        j["projects"] = nlohmann::json::array();
        for (auto& kp : projects)
            j["projects"].push_back({
                {"name",       kp.name},
                {"path",       kp.path.string()},
                {"lastOpened", kp.lastOpened},
            });
        std::ofstream f(file());
        f << j.dump(2) << "\n";
    }

    // Upsert + bump lastOpened. Call whenever a project is opened or created.
    static void touch(const std::string& name,
                      const std::filesystem::path& path) {
        auto list = load();
        const long long now = (long long)std::chrono::duration_cast<
            std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto it = std::find_if(list.begin(), list.end(),
            [&](const KnownProject& kp) { return kp.path == path; });
        if (it != list.end()) { it->name = name; it->lastOpened = now; }
        else                  { list.push_back({name, path, now}); }
        save(list);
    }

    static void remove(const std::filesystem::path& path) {
        auto list = load();
        list.erase(std::remove_if(list.begin(), list.end(),
            [&](const KnownProject& kp) { return kp.path == path; }),
            list.end());
        save(list);
    }
};

} // namespace project
