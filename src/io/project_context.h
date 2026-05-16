#pragma once
#include <filesystem>
#include <string>
#include <fstream>
#include <nlohmann/json.hpp>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

struct ProjectContext {
    std::filesystem::path projectRoot;   // folder containing project.json
    std::filesystem::path assetsRoot;    // projectRoot/assets by default
    std::string           name          = "Untitled Project";
    std::string           lastScene     = "scenes/main.scene";
    int                   version       = 1;

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

        nlohmann::json j;
        j["name"]        = name;
        j["version"]     = version;
        j["assetRoot"]   = std::filesystem::relative(assetsRoot, projectRoot).string();
        j["lastScene"]   = lastScene;

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

    // Last-opened project stored in user home for quick resume.
    static std::filesystem::path lastProjectFile() {
        auto home = std::filesystem::path(
            getenv("HOME") ? getenv("HOME") : ".");
        auto dir = home / ".engine";
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
