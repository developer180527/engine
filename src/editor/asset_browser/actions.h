#pragma once
// Asset-browser filesystem actions: create folder / script, rename, delete,
// duplicate, reveal-in-Finder, and the "is this a viewable text file?" set.
#include <filesystem>
#include <fstream>
#include <string>
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

// macOS: reveal in Finder. Single-quotes the path (escaping embedded quotes).
inline void revealInFinder(const std::filesystem::path& p) {
    std::string raw = p.string(), q;
    q.reserve(raw.size() + 2);
    for (char c : raw) { if (c=='\'') q += "'\\''"; else q += c; }
    bool isDir = std::filesystem::is_directory(p);
    std::string cmd = (isDir ? "open '" : "open -R '") + q + "'";
    std::system(cmd.c_str());
}

inline bool isViewableText(const std::string& e) {
    static const std::unordered_set<std::string> s = {
        ".lua",".py",".cpp",".cc",".cxx",".c",".h",".hpp",".hxx",".inl",
        ".glsl",".vert",".frag",".comp",".shader",".json",".txt",".md",
        ".xml",".yaml",".yml",".ini",".cfg",".toml",".csv",".log"};
    return s.count(e) > 0;
}

} // namespace ab
