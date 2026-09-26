#pragma once
#include <string>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <assetlib/asset_registry.h>
#include <cstdio>
#include <filesystem>

namespace ab {

enum class ViewMode { Grid, List };

// A colour the browser's data carries (icon styles, registry-state badges),
// 8-bit straight RGBA. Plain data rather than ImU32, so the model and these
// tables stay GUI-toolkit-free; each GUI converts at its drawing edge.
struct Rgba { uint8_t r = 0, g = 0, b = 0, a = 255; };

struct RegistryInfo {
    bool                   found       = false;
    std::string            uuid;
    assetlib::AssetState   state       = assetlib::AssetState::Unknown;
    uint32_t               cookVersion = 0;
    int64_t                cookedAt    = 0;
    uintmax_t              cookedBytes = 0;
    std::string            cookedPath;
};

struct FileEntry {
    std::string  name;
    std::string  fullPath;
    std::string  ext;          // lowercase e.g. ".fbx"
    uintmax_t    sizeBytes = 0;
    bool         supported = false;
    bool         loaded    = false;
    bool         isDir     = false;
    RegistryInfo reg;
};

// ── Formatters ───────────────────────────────────────────────────────────────
inline std::string formatSize(uintmax_t b) {
    char buf[32];
    if      (b >= 1ULL<<30) std::snprintf(buf,sizeof(buf),"%.1f GB",b/(double)(1<<30));
    else if (b >= 1<<20)    std::snprintf(buf,sizeof(buf),"%.1f MB",b/(double)(1<<20));
    else if (b >= 1<<10)    std::snprintf(buf,sizeof(buf),"%.1f KB",b/(double)(1<<10));
    else                    std::snprintf(buf,sizeof(buf),"%llu B",(unsigned long long)b);
    return buf;
}

inline std::string formatTime(int64_t ts) {
    if (ts == 0) return "never";
    std::time_t t = (std::time_t)ts;
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", std::localtime(&t));
    return buf;
}

inline std::string baseName(const std::string& fn) {
    auto d = fn.find_last_of('.');
    return d != std::string::npos ? fn.substr(0,d) : fn;
}

inline std::string lowerExt(const std::filesystem::path& p) {
    std::string e = p.extension().string();
    for (auto& c : e) c = (char)std::tolower(c);
    return e;
}

// ── Icon styles ──────────────────────────────────────────────────────────────
struct IconStyle { Rgba bg; Rgba fg; const char* label; };

inline IconStyle iconStyle(const std::string& ext, bool isDir = false) {
    if (isDir)
        return {Rgba{190,155,45,255}, Rgba{255,230,130,255}, "DIR"};
    if (ext==".fbx"||ext==".obj"||ext==".dae"||ext==".ply"||ext==".stl")
        return {Rgba{200,110,40,255}, Rgba{255,200,140,255}, "MESH"};
    if (ext==".glb"||ext==".gltf")
        return {Rgba{50,110,210,255}, Rgba{160,200,255,255}, "GLTF"};
    if (ext==".png"||ext==".jpg"||ext==".jpeg"||ext==".tga"||ext==".bmp")
        return {Rgba{140,60,190,255}, Rgba{220,170,255,255}, "TEX"};
    if (ext==".hdr"||ext==".exr")
        return {Rgba{40,160,160,255}, Rgba{160,240,240,255}, "HDR"};
    if (ext==".scene"||ext==".json")
        return {Rgba{50,150,80,255},  Rgba{160,240,180,255}, "SCN"};
    return      {Rgba{70,70,70,255},  Rgba{180,180,180,255}, "FILE"};
}

} // namespace ab
