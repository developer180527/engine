#pragma once

#include <filesystem>
#include <string>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#elif defined(_WIN32)
    #include <windows.h>
#endif

// ProjectContext: the engine's knowledge of where it is and what it owns.
//
// Currently minimal — just the assets root path. Future milestones will
// expand this to include:
//   - Project file path and metadata (name, version, description)
//   - Imported asset cache directory (engine-native binary cache)
//   - Editor settings path (panel layout, recently opened projects)
//   - Per-project shader/compiler settings
//
// Replaces the old asset_path.h which had ad-hoc path resolution baked
// into free functions. ProjectContext is a proper home for this logic that
// can be passed around, serialized, and extended without breaking callers.
struct ProjectContext {
    std::filesystem::path assetsRoot;

    // Returns true if assetsRoot points to an existing directory.
    bool valid() const {
        return !assetsRoot.empty()
            && std::filesystem::is_directory(assetsRoot);
    }

    // Resolve a filename relative to the assets root to a full path.
    // Returns the full path whether or not the file exists — callers
    // check existence themselves so they can produce informative errors.
    std::string resolve(const std::string& relPath) const {
        auto full = assetsRoot / relPath;
        if (std::filesystem::exists(full))
            return std::filesystem::weakly_canonical(full).string();
        return full.string();
    }

    // Auto-detect the assets root relative to the running executable.
    // Tries two layouts:
    //   1. <exe>/../assets/   — dev layout (build/engine + repo/assets)
    //   2. <exe>/assets/      — installed/shipped layout
    // Falls back to CWD/assets/ if neither exists.
    static ProjectContext autoDetect() {
        ProjectContext ctx;
        namespace fs = std::filesystem;

        const fs::path exeDir = getExeDir();

        // Dev layout: executable is one level inside the project root.
        const fs::path dev = exeDir / ".." / "assets";
        if (fs::is_directory(dev)) {
            ctx.assetsRoot = fs::weakly_canonical(dev);
            return ctx;
        }

        // Installed layout: assets/ sits next to the executable.
        const fs::path installed = exeDir / "assets";
        if (fs::is_directory(installed)) {
            ctx.assetsRoot = fs::weakly_canonical(installed);
            return ctx;
        }

        // Fallback: wherever we were launched from.
        ctx.assetsRoot = fs::current_path() / "assets";
        return ctx;
    }

private:
    static std::filesystem::path getExeDir() {
        namespace fs = std::filesystem;
#if defined(__APPLE__)
        char buf[1024];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0)
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
