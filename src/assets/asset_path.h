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
//
// Runs inside a static initializer, so it must NEVER let an exception escape:
// an uncaught throw here (e.g. fs::canonical failing under a chroot/flatpak
// where /proc is masked, or a path that won't resolve) calls std::terminate
// before main() even begins. Every filesystem call uses the error_code
// overloads, and the whole body is wrapped so any surprise degrades to the
// current-working-directory fallback instead of killing the process.
inline const std::filesystem::path& executableDir() {
    static const std::filesystem::path dir = []() -> std::filesystem::path {
        namespace fs = std::filesystem;
        std::error_code ec;
        try {
#if defined(__APPLE__)
            // _NSGetExecutablePath needs a buffer of the exact size; on
            // overflow it returns non-zero and writes the required length.
            uint32_t size = 1024;
            std::string buf(size, '\0');
            if (_NSGetExecutablePath(buf.data(), &size) != 0) {
                buf.resize(size);
                if (_NSGetExecutablePath(buf.data(), &size) != 0)
                    return fs::current_path(ec);
            }
            buf.resize(std::char_traits<char>::length(buf.c_str()));
            auto c = fs::canonical(buf, ec);
            if (!ec) return c.parent_path();
#elif defined(__linux__)
            auto c = fs::canonical("/proc/self/exe", ec);
            if (!ec) return c.parent_path();
#elif defined(_WIN32)
            // Wide-char + growing buffer: GetModuleFileNameA mangles non-ASCII
            // install paths and, on truncation, does not null-terminate. The W
            // variant with an explicit truncation check (n == size) avoids both.
            std::wstring buf(1024, L'\0');
            for (;;) {
                DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
                if (n == 0) break;                      // hard failure
                if (n < buf.size()) {                   // fully written + terminated
                    buf.resize(n);
                    auto c = fs::canonical(buf, ec);
                    if (!ec) return c.parent_path();
                    break;
                }
                if (buf.size() >= (1u << 16)) break;    // absurd — give up
                buf.resize(buf.size() * 2);             // truncated — grow, retry
            }
#endif
        } catch (...) { /* fall through to CWD */ }
        auto cwd = fs::current_path(ec);
        return ec ? fs::path{} : cwd;                   // last-resort fallback
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
