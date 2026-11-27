#pragma once
// ── device_select — which graphics backend, and whether the device will do ───
// WO-089, docs/plans/provider-selection.md §2 (DR-0037).
//
// The graphics API is not a provider. It is a backend inside the renderer's
// device layer, compiled per platform. Two choices are left for startup,
// because only the device knows them:
//
//   1. WHICH backend creates a device. The project may order the ones its
//      platform has, in project.json:
//
//        "rhi": { "windows": ["d3d12", "vulkan"], "linux": ["vulkan"] }
//
//      The list is the only fallback: each is tried in turn, and past its end
//      the renderer refuses by name with what was tried. Leaving a platform
//      out takes the FIRST backend of its table's "gpu_apis" and only that:
//      the table lists what the platform has, not a fallback chain, so a
//      project that never wrote "rhi" gets one backend, as before WO-089.
//
//   2. WHETHER the device has what the renderer needs. A renderer's manifest
//      names each device capability it uses as either needed (missing: refused
//      by name) or with a fallback (missing: the fallback, logged). A
//      capability the manifest does not name is not checked.
//
// The same functions run at engine_build (through the resolver, against every
// platform table) and at startup in every player, shipped ones included. They
// are here, not in the resolver, because a shipped player contains no resolver
// (DR-0032) but still reads its project's "rhi" order.
//
// Pure: the standard library and nlohmann::json.
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rhisel {

// Every backend name the engine knows. A platform table's "gpu_apis" and a
// project's "rhi" lists use these names; the renderer maps them to its own.
inline const std::vector<std::string>& backends() {
    static const std::vector<std::string> b = {"d3d12", "d3d11", "vulkan", "metal", "opengl", "gles", "webgpu", "webgl2"};
    return b;
}

// Device capabilities a renderer may name, each with what it means. Names are
// the engine's, not one graphics API's.
struct Capability { const char* name; const char* what; };
inline const std::vector<Capability>& capabilities() {
    static const std::vector<Capability> c = {
        {"index32",            "32-bit index buffers"},
        {"depth_d32f",         "a 32-bit float depth texture that can be rendered to (the shadow map)"},
        {"instancing",         "instanced draws"},
        {"render_rgba16f",     "RGBA16F colour targets that can be rendered to (the HDR scene target)"},
        {"texture3d_rgba16f",  "RGBA16F 3D textures (colour-grading LUTs)"},
        {"compute",            "compute shaders"},
        {"texture_2d_array",   "2D texture arrays"},
    };
    return c;
}

inline bool isCapability(const std::string& n) {
    for (const Capability& c : capabilities()) if (n == c.name) return true;
    return false;
}

namespace detail {
inline bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
inline std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) { if (!s.empty()) s += ", "; s += x; }
    return s.empty() ? "none" : s;
}
} // namespace detail

// ── 1. The backend order ─────────────────────────────────────────────────────

// Checks a project's "rhi" block against the platforms whose backends are
// known (name -> its "gpu_apis"), and returns each problem by name. A null
// block is no block. A key for a platform not in `known` is checked for shape
// only; its backends are checked wherever that platform's table is read.
inline std::vector<std::string> checkRhi(const nlohmann::json& rhi,
                                         const std::vector<std::string>& allPlatforms,
                                         const std::map<std::string, std::vector<std::string>>& known) {
    std::vector<std::string> out;
    if (rhi.is_null()) return out;
    if (!rhi.is_object()) {
        out.push_back("project.json \"rhi\" must be an object of platform: [backends, in order]");
        return out;
    }
    for (auto it = rhi.begin(); it != rhi.end(); ++it) {
        const std::string& p = it.key();
        const std::string where = "project.json \"rhi\"." + p;
        if (!detail::has(allPlatforms, p)) {
            out.push_back("project.json \"rhi\": '" + p + "' is not a platform (platforms: " +
                          detail::join(allPlatforms) + ")");
            continue;
        }
        if (!it->is_array() || it->empty()) {
            out.push_back(where + " must be a non-empty list of backends, in order");
            continue;
        }
        std::vector<std::string> seen;
        for (const auto& e : *it) {
            if (!e.is_string()) { out.push_back(where + " must be a list of backend names"); break; }
            const std::string b = e.get<std::string>();
            if (detail::has(seen, b)) { out.push_back(where + ": '" + b + "' is listed twice"); continue; }
            seen.push_back(b);
            if (!detail::has(backends(), b)) {
                out.push_back(where + ": '" + b + "' is not a backend (backends: " + detail::join(backends()) + ")");
                continue;
            }
            auto k = known.find(p);
            if (k != known.end() && !detail::has(k->second, b))
                out.push_back(where + ": '" + b + "' is not built for " + p + " (" + p + " has " +
                              detail::join(k->second) + ")");
        }
    }
    return out;
}

// The order to try on `platform`: the project's list when it gives one, else
// the platform's first backend alone (`platformApis` is its table's list).
// Call checkRhi first; this assumes a valid block.
inline std::vector<std::string> orderFor(const nlohmann::json& rhi, const std::string& platform,
                                         const std::vector<std::string>& platformApis,
                                         std::string* chosenBy = nullptr) {
    if (rhi.is_object()) {
        auto it = rhi.find(platform);
        if (it != rhi.end() && it->is_array()) {
            std::vector<std::string> v;
            for (const auto& e : *it) if (e.is_string()) v.push_back(e.get<std::string>());
            if (chosenBy) *chosenBy = "project.json \"rhi\"." + platform;
            return v;
        }
    }
    if (chosenBy) *chosenBy = "the " + platform + " default";
    if (platformApis.empty()) return {};
    return {platformApis.front()};
}

struct Attempt { std::string backend, whyNot; };
struct TryResult {
    std::string used;                 // empty: none created a device
    std::vector<Attempt> failed;      // each earlier one, and why not
    std::string refusal;              // set when `used` is empty
    bool ok() const { return !used.empty(); }
};

// Tries each backend in `order`. `attempt` creates a device on one backend and
// returns empty on success, or why not. The first success wins; nothing past
// the list is tried.
inline TryResult tryInOrder(const std::vector<std::string>& order, const std::string& chosenBy,
                            const std::function<std::string(const std::string&)>& attempt) {
    TryResult r;
    for (const std::string& b : order) {
        std::string why = attempt(b);
        if (why.empty()) { r.used = b; return r; }
        r.failed.push_back({b, std::move(why)});
    }
    std::string tried;
    for (const Attempt& a : r.failed) { if (!tried.empty()) tried += "; "; tried += a.backend + ": " + a.whyNot; }
    r.refusal = "no graphics backend in the order (" + chosenBy + ": " + detail::join(order) +
                ") created a device — " + (tried.empty() ? std::string("the list is empty") : tried);
    return r;
}

// ── 2. The device's capabilities ─────────────────────────────────────────────

// What a renderer's manifest says about the device: `needs` are required,
// `fallbacks` are met by the renderer's fallback when missing.
struct DeviceDecl { std::vector<std::string> needs, fallbacks; };

// Problems with a manifest's "device" block; empty when it is right.
inline std::string checkDeviceDecl(const DeviceDecl& d) {
    for (const std::vector<std::string>* l : {&d.needs, &d.fallbacks})
        for (const std::string& c : *l)
            if (!isCapability(c)) {
                std::vector<std::string> names;
                for (const Capability& k : capabilities()) names.push_back(k.name);
                return "'" + c + "' is not a device capability (capabilities: " + detail::join(names) + ")";
            }
    for (const std::string& c : d.needs)
        if (detail::has(d.fallbacks, c))
            return "'" + c + "' is both needed and given a fallback; it must be one or the other";
    return {};
}

// What the engine hands a renderer about its device (ProviderContext::device).
struct DeviceRequest {
    std::vector<std::string> backendOrder;
    std::string              orderChosenBy;   // for the log and the refusal
    DeviceDecl               decl;
};

struct CapResult {
    std::vector<std::string> missingNeeds;      // refused
    std::vector<std::string> fellBack;          // met by the renderer's fallback
    bool ok() const { return missingNeeds.empty(); }
    std::string refusal(const std::string& renderer, const std::string& backend) const {
        std::string s = "the renderer '" + renderer + "' needs ";
        for (size_t i = 0; i < missingNeeds.size(); ++i) {
            if (i) s += ", ";
            s += "'" + missingNeeds[i] + "'";
            for (const Capability& c : capabilities()) if (missingNeeds[i] == c.name) s += std::string(" (") + c.what + ")";
        }
        return s + ", which this " + backend + " device does not have";
    }
};

// Holds the device's capabilities (`have`) against the manifest's.
inline CapResult checkDevice(const DeviceDecl& d, const std::set<std::string>& have) {
    CapResult r;
    for (const std::string& c : d.needs)     if (!have.count(c)) r.missingNeeds.push_back(c);
    for (const std::string& c : d.fallbacks) if (!have.count(c)) r.fellBack.push_back(c);
    return r;
}

} // namespace rhisel
