#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

struct ProjectContext {
    // A reusable C++ gameplay kit this project plugs in (FPS controller, IK,
    // water, ...). Kits live in their own repos, built ON the engine; the game
    // names the module here and the runtime dlopens it lazily at Play.
    //   module — path to the loadable .so, absolute or relative to projectRoot
    //   enabled — skip the load without deleting the entry
    //   requiresKits — names of kits that must LOAD BEFORE this one (JSON key
    //     "requires"). Ordering only — kits never link each other's code; this
    //     exists for service-publish patterns where A must start before B reads.
    struct Kit {
        std::string name;
        std::string module;
        bool        enabled = true;
        std::vector<std::string> requiresKits;
    };

    std::filesystem::path projectRoot;   // folder containing project.json
    std::filesystem::path assetsRoot;    // projectRoot/assets by default
    std::string           name          = "Untitled Project";
    std::string           lastScene     = "scenes/main.scene";
    int                   version       = 1;
    std::vector<Kit>      kits;          // plugged-in gameplay kits (see Kit)

    bool valid() const {
        return !assetsRoot.empty()
            && std::filesystem::is_directory(assetsRoot);
    }

    std::string resolve(const std::string& rel) const {
        auto full = assetsRoot / rel;
        return std::filesystem::exists(full)
            ? std::filesystem::weakly_canonical(full).string()
            : full.string();
    }

    // ---- Persistence ------------------------------------------------

    // Save project.json into projectRoot.
    bool save() const {
        if (projectRoot.empty()) return false;
        std::filesystem::create_directories(projectRoot);
        std::filesystem::create_directories(projectRoot / "scenes");

        // Preserve keys we don't model (e.g. "engine", "template") by loading
        // the existing file first and overwriting only the fields we own.
        nlohmann::json j;
        {
            std::ifstream in(projectRoot / "project.json");
            if (in) { try { in >> j; } catch (...) { j = nlohmann::json::object(); } }
        }
        j["name"]        = name;
        j["version"]     = version;
        j["assetRoot"]   = std::filesystem::relative(assetsRoot, projectRoot).string();
        j["lastScene"]   = lastScene;
        nlohmann::json jk = nlohmann::json::array();
        for (const auto& k : kits) {
            nlohmann::json e = {{"name", k.name}, {"module", k.module},
                                {"enabled", k.enabled}};
            if (!k.requiresKits.empty()) e["requires"] = k.requiresKits;
            jk.push_back(std::move(e));
        }
        j["kits"] = std::move(jk);

        std::ofstream f(projectRoot / "project.json");
        f << j.dump(2);
        return f.good();
    }

    // Load from a project folder (must contain project.json).
    static ProjectContext load(const std::filesystem::path& root) {
        ProjectContext ctx;
        ctx.projectRoot = std::filesystem::weakly_canonical(root);

        auto pjPath = ctx.projectRoot / "project.json";
        if (std::filesystem::exists(pjPath)) {
            try {
                std::ifstream _pjf(pjPath);
            nlohmann::json j = nlohmann::json::parse(_pjf);
                ctx.name       = j.value("name",      "Untitled Project");
                ctx.version    = j.value("version",   1);
                ctx.lastScene  = j.value("lastScene", "scenes/main.scene");
                std::string ar = j.value("assetRoot", "assets");
                ctx.assetsRoot = std::filesystem::weakly_canonical(ctx.projectRoot / ar);
                if (auto it = j.find("kits"); it != j.end() && it->is_array()) {
                    for (const auto& jk : *it) {
                        Kit k;
                        k.name    = jk.value("name",    std::string{});
                        k.module  = jk.value("module",  std::string{});
                        k.enabled = jk.value("enabled", true);
                        if (auto r = jk.find("requires"); r != jk.end() && r->is_array())
                            for (const auto& dep : *r)
                                if (dep.is_string()) k.requiresKits.push_back(dep.get<std::string>());
                        if (!k.module.empty()) ctx.kits.push_back(std::move(k));
                    }
                }
            } catch (...) {
                ctx.assetsRoot = ctx.projectRoot / "assets";
            }
        } else {
            // New project — create default layout
            ctx.assetsRoot = ctx.projectRoot / "assets";
            std::filesystem::create_directories(ctx.assetsRoot);
            std::filesystem::create_directories(ctx.projectRoot / "scenes");
            ctx.save();
        }
        return ctx;
    }

    // Cross-platform user home directory (HOME on Unix/macOS, USERPROFILE or
    // HOMEDRIVE+HOMEPATH on Windows; "." as a last resort).
    static std::filesystem::path homeDir() {
#if defined(_WIN32)
        if (const char* up = getenv("USERPROFILE")) return up;
        const char* hd = getenv("HOMEDRIVE");
        const char* hp = getenv("HOMEPATH");
        if (hd && hp) return std::filesystem::path(hd) / hp;
        return ".";
#else
        const char* h = getenv("HOME");
        return h ? std::filesystem::path(h) : std::filesystem::path(".");
#endif
    }

    // Last-opened project stored in user home for quick resume.
    static std::filesystem::path lastProjectFile() {
        auto dir = homeDir() / ".engine";
        std::filesystem::create_directories(dir);
        return dir / "last_project.txt";
    }

    void saveAsLastProject() const {
        std::ofstream f(lastProjectFile());
        f << projectRoot.string();
    }

    static std::filesystem::path loadLastProjectPath() {
        auto p = lastProjectFile();
        if (!std::filesystem::exists(p)) return {};
        std::ifstream f(p);
        std::string path;
        std::getline(f, path);
        return path.empty() ? std::filesystem::path{} : std::filesystem::path{path};
    }

    // Legacy fallback — used when no project is specified.
    static ProjectContext autoDetect() {
        auto last = loadLastProjectPath();
        if (!last.empty() && std::filesystem::is_directory(last))
            return load(last);

        // Dev layout: exe is in build/, project root is one level up
        namespace fs = std::filesystem;
        const fs::path exeDir = getExeDir();
        const fs::path dev    = exeDir / "..";
        if (fs::is_directory(dev / "assets"))
            return load(dev);

        return load(fs::current_path());
    }

private:
    static std::filesystem::path getExeDir() {
        namespace fs = std::filesystem;
#if defined(__APPLE__)
        char buf[1024]; uint32_t sz = sizeof(buf);
        if (_NSGetExecutablePath(buf, &sz) == 0)
            return fs::canonical(buf).parent_path();
#elif defined(__linux__)
        return fs::canonical("/proc/self/exe").parent_path();
#elif defined(_WIN32)
        char buf[1024];
        GetModuleFileNameA(nullptr, buf, sizeof(buf));
        return fs::canonical(buf).parent_path();
#endif
        return fs::current_path();
    }
};
