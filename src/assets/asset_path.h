#pragma once

#include <string>
#include <filesystem>

#if defined(__APPLE__)
    #include <mach-o/dyld.h>     // _NSGetExecutablePath
#elif defined(_WIN32)
    #include <windows.h>          // GetModuleFileNameA
#endif
// Linux uses /proc/self/exe via std::filesystem::canonical, no extra include.

// Resolves asset paths relative to the engine executable, NOT the current
// working directory.
//
// Why: when you run `./build/engine` from the repo root, CWD is the repo
// root. When you run it from `build/` directly, CWD is `build/`. When the
// engine is eventually packaged and shipped, CWD is wherever the user
// double-clicked from. None of those are reliable.
//
// The executable's location IS reliable — it's wherever the binary was
// installed/built to. Assets shipped alongside the binary are findable
// from there.
//
// Resolution order:
//   1. <exe_dir>/assets/<path>     (when binary lives next to assets/)
//   2. <exe_dir>/../assets/<path>  (during dev: build/engine + repo/assets)

namespace asset_path {

// Cached at first call.
inline const std::filesystem::path& executableDir() {
    static const std::filesystem::path dir = []() -> std::filesystem::path {
        namespace fs = std::filesystem;

#if defined(__APPLE__)
        char buf[1024];
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0) {
            return fs::canonical(buf).parent_path();
        }
#elif defined(__linux__)
        return fs::canonical("/proc/self/exe").parent_path();
#elif defined(_WIN32)
        char buf[1024];
        GetModuleFileNameA(nullptr, buf, sizeof(buf));
        return fs::canonical(buf).parent_path();
#endif
        return fs::current_path();  // fallback
    }();
    return dir;
}

// Resolve a path like "Duck.gltf" or "models/character.glb" to a full path.
// Tries <exe_dir>/assets/<rel>, then <exe_dir>/../assets/<rel>.
inline std::string resolve(const std::string& relativeToAssets) {
    namespace fs = std::filesystem;

    const fs::path exeDir = executableDir();

    const fs::path candidate1 = exeDir / "assets" / relativeToAssets;
    if (fs::exists(candidate1)) {
        return candidate1.string();
    }

    const fs::path candidate2 = exeDir / ".." / "assets" / relativeToAssets;
    if (fs::exists(candidate2)) {
        return fs::weakly_canonical(candidate2).string();
    }

    // Return the first candidate even though it doesn't exist — caller
    // reports a clean "not found" error referencing this path.
    return candidate1.string();
}

} // namespace asset_path
