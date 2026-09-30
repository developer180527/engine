#pragma once
// Asset-browser filesystem actions: create folder / script, rename, delete,
// duplicate, reveal-in-Finder, and the "is this a viewable text file?" set.
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace ab {

enum class NewKind { None, Folder, ScriptLua, ScriptPython, ScriptCpp };

inline const char* scriptExt(NewKind k) {
    switch (k) { case NewKind::ScriptLua: return ".lua";
                 case NewKind::ScriptPython: return ".py";
                 case NewKind::ScriptCpp: return ".cpp"; default: return ""; }
}

inline std::string scriptTemplate(NewKind k, const std::string& stem) {
    switch (k) {
    case NewKind::ScriptLua:
        return "-- " + stem + ".lua\n"
               "local M = {}\n\n"
               "function M:onStart()\n"
               "    -- self.entity is this entity's handle\n"
               "end\n\n"
               "function M:onUpdate(dt)\n"
               "end\n\n"
               "return M\n";
    case NewKind::ScriptPython:
        return "# " + stem + ".py\n\n"
               "def on_start(self):\n"
               "    pass\n\n"
               "def on_update(self, dt):\n"
               "    pass\n";
    case NewKind::ScriptCpp:
        return "// " + stem + ".cpp\n\n"
               "#include <cstdint>\n\n"
               "// TODO: implement\n";
    default: return "";
    }
}

inline bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() &&
           s.compare(s.size()-suf.size(), suf.size(), suf) == 0;
}

inline bool createFolder(const std::filesystem::path& dir, const std::string& name) {
    std::error_code ec;
    return std::filesystem::create_directory(dir / name, ec) && !ec;
}

// Writes <name><ext> (ext appended if missing). Won't clobber. Returns path or "".
inline std::string createScript(const std::filesystem::path& dir,
                                const std::string& name, NewKind kind) {
    std::string ext = scriptExt(kind);
    std::string fn  = name;
    if (ext[0] && !endsWith(fn, ext)) fn += ext;
    auto full = dir / fn;
    if (std::filesystem::exists(full)) return "";
    std::string stem = std::filesystem::path(fn).stem().string();
    std::ofstream f(full, std::ios::binary);
    if (!f) return "";
    f << scriptTemplate(kind, stem);
    return full.string();
}

inline bool renamePath(const std::filesystem::path& oldPath, const std::string& newName) {
    std::error_code ec;
    std::filesystem::rename(oldPath, oldPath.parent_path() / newName, ec);
    return !ec;
}

inline bool deletePath(const std::filesystem::path& p) {
    std::error_code ec;
    if (std::filesystem::is_directory(p)) { std::filesystem::remove_all(p, ec); return !ec; }
    std::filesystem::remove(p, ec);
    return !ec;
}

inline std::string duplicatePath(const std::filesystem::path& p) {
    if (std::filesystem::is_directory(p)) return ""; // folder dup skipped for now
    auto stem = p.stem().string(); auto ext = p.extension().string();
    auto dir  = p.parent_path();
    for (int i = 1; i < 1000; ++i) {
        std::string cand = stem + " copy" + (i==1 ? "" : std::to_string(i)) + ext;
        auto full = dir / cand;
        if (!std::filesystem::exists(full)) {
            std::error_code ec;
            std::filesystem::copy_file(p, full, ec);
            return ec ? "" : full.string();
        }
    }
    return "";
}

// ── Reveal in the OS file manager (WO-005) ───────────────────────────────────
// This used to run `open -R '…'` on EVERY OS. On Linux `open` is a different
// program (openvt, or nothing), so the menu item did something unrelated or
// nothing, silently. Now each OS gets its own file manager, and one we do not
// know says so and greys the menu item out. Moves behind os:: with WO-021.
//
// The command is built by a PURE function, with the OS as a parameter, so one
// test checks all three commands and their quoting on whichever machine it
// runs on. Only revealInFileManager() depends on the host.
enum class RevealOs { Apple, Windows, Linux, Unknown };

constexpr RevealOs hostRevealOs() {
#if defined(__APPLE__)
    return RevealOs::Apple;
#elif defined(_WIN32)
    return RevealOs::Windows;
#elif defined(__linux__)
    return RevealOs::Linux;
#else
    return RevealOs::Unknown;
#endif
}

// What the menu item is called where the user is.
constexpr const char* revealLabel(RevealOs os = hostRevealOs()) {
    switch (os) {
        case RevealOs::Apple:   return "Reveal in Finder";
        case RevealOs::Windows: return "Show in Explorer";
        default:                return "Open Containing Folder";
    }
}

constexpr bool canReveal(RevealOs os = hostRevealOs()) { return os != RevealOs::Unknown; }

// The shell command, or "" when it cannot be built safely. POSIX shells get the
// path in single quotes (an embedded ' becomes '\''), so nothing in a filename
// is interpreted. Windows paths cannot contain `"`, so a path with one is
// refused rather than escaped. A file is selected in its folder where the file
// manager can do that (Finder, Explorer); xdg-open cannot select, so Linux opens
// the containing folder. Linux runs it in the background: xdg-open may block
// for as long as the file manager stays open, and this is called on the UI thread.
inline std::string revealCommand(const std::string& path, bool isDir, RevealOs os) {
    auto shq = [](const std::string& raw) {
        std::string q = "'";
        for (char c : raw) { if (c == '\'') q += "'\\''"; else q += c; }
        return q + "'";
    };
    switch (os) {
        case RevealOs::Apple:
            return (isDir ? "open " : "open -R ") + shq(path);
        case RevealOs::Windows:
            if (path.find('"') != std::string::npos) return "";
            return (isDir ? "explorer \"" : "explorer /select,\"") + path + "\"";
        case RevealOs::Linux: {
            const std::string dir = isDir ? path
                : std::filesystem::path(path).parent_path().string();
            return "xdg-open " + shq(dir.empty() ? "." : dir) + " >/dev/null 2>&1 &";
        }
        case RevealOs::Unknown: break;
    }
    return "";
}

// true when the command was ISSUED; false when this OS has no known file
// manager or the command cannot be built (the first such failure is printed
// once). Not "the window opened": explorer.exe exits 1 even when it succeeds,
// and a backgrounded xdg-open always returns 0, so the exit code means nothing.
inline bool revealInFileManager(const std::filesystem::path& p) {
    std::string cmd;
    try {
        cmd = revealCommand(p.string(), std::filesystem::is_directory(p), hostRevealOs());
    } catch (const std::exception&) {
        cmd.clear();   // p.string() throws on Windows for a name the code page cannot hold
    }
    if (cmd.empty()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr, "[AssetBrowser] cannot reveal %s in a file manager on this OS\n",
                         p.filename().string().c_str());
        }
        return false;
    }
    (void)std::system(cmd.c_str());
    return true;
}

inline bool isViewableText(const std::string& e) {
    static const std::unordered_set<std::string> s = {
        ".lua",".py",".cpp",".cc",".cxx",".c",".h",".hpp",".hxx",".inl",
        ".glsl",".vert",".frag",".comp",".shader",".json",".txt",".md",
        ".xml",".yaml",".yml",".ini",".cfg",".toml",".csv",".log"};
    return s.count(e) > 0;
}

} // namespace ab
