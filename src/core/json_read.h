#pragma once
// ── json_read — bounds- and type-safe reads out of nlohmann::json ────────────
//
// EXISTS BECAUSE THE SAME BUG WAS WRITTEN FOUR TIMES. nlohmann's CONST
// `operator[](size_type)` is UNDEFINED BEHAVIOUR out of range. It is not
// `at()`: no bounds check, no exception, no `catch` that can save you. It
// indexes the underlying std::vector directly and hands back a reference to
// nothing, so `j["position"][0]` on an EMPTY array dereferences null.
//
// Found in four places in one pass, all reading a float triple out of a file a
// human can edit:
//   • `scene/entity_serializer.h`   — five sites (position, rotation, scale,
//     clearColor, halfExtent, color), reached from every `.scene` load
//   • `editor/editor_prefs.h`       — `editor.json`, on the project-OPEN path,
//     so the crash means the project cannot be opened at all
//   • `editor/undo_stack.h`         — `desTf`, latent today because its JSON is
//     built in-process, one hand-authored command away from live
//
// And the matching type hazard: `j.value(key, default)` does NOT fall back to
// the default when the key exists with the WRONG type — it throws. So a plain
// read is unsafe in both directions, and the two failure modes hide each other
// (the throw gets there first, so the UB only surfaces once you fix the throw).
//
// NON-FINITE VALUES ARE REJECTED TOO. JSON has no NaN literal, but `1e999`
// parses to +inf through strtod, and an infinite scale or fov propagates into
// every world matrix downstream — corrupting a frame nowhere near the load that
// caused it. Dropping it at the boundary is the only place the bad file is
// still nameable.
//
// The contract everywhere: a missing, wrong-typed, short, or non-finite value
// leaves the destination ALONE. Callers pass a meaningful default in; they
// never get a zero they did not ask for. That matters — a scale of {1,1,0}
// from a two-element array collapses an object to a plane, which is far harder
// to recognise than a value that simply was not overridden.
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <string>

namespace jsonread {

// True (and writes `out`) only for a real, finite number.
inline bool finiteNumber(const nlohmann::json& v, float& out) {
    if (!v.is_number()) return false;
    const float f = v.get<float>();
    if (!std::isfinite(f)) return false;
    out = f;
    return true;
}

// Read up to `count` floats from `j[key]`, which must be an array. Elements
// past the array's end, and elements that are not finite numbers, leave the
// corresponding `out` slot untouched.
inline void readFloats(const nlohmann::json& j, const char* key,
                       float* out, int count) {
    if (!j.is_object() || !j.contains(key)) return;
    const auto& a = j[key];
    if (!a.is_array()) return;
    const int n = (int)a.size() < count ? (int)a.size() : count;
    for (int i = 0; i < n; ++i) {
        float f;
        if (finiteNumber(a[(size_t)i], f)) out[i] = f;
    }
}

// Scalar counterpart: the default survives a missing, wrong-typed or
// non-finite value instead of throwing or propagating an infinity.
inline float readFloat(const nlohmann::json& j, const char* key, float dflt) {
    if (!j.is_object() || !j.contains(key)) return dflt;
    float f;
    return finiteNumber(j[key], f) ? f : dflt;
}

// Integer counterparts, for the same reason `readFloat` exists: `j.value(key,
// 0u)` throws when the key is present with the wrong type, and a `"id": "3"` in
// a hand-edited scene should not be an exception escaping a cook thread.
inline uint64_t readU64(const nlohmann::json& j, const char* key, uint64_t dflt) {
    if (!j.is_object() || !j.contains(key)) return dflt;
    const auto& v = j[key];
    return v.is_number_unsigned() || v.is_number_integer() ? v.get<uint64_t>() : dflt;
}

inline uint32_t readU32(const nlohmann::json& j, const char* key, uint32_t dflt) {
    const uint64_t v = readU64(j, key, dflt);
    return v > 0xFFFFFFFFull ? dflt : (uint32_t)v;
}

// A string, or the default. `j.value(key, std::string{})` has the same throwing
// behaviour on a wrong-typed key.
inline std::string readString(const nlohmann::json& j, const char* key,
                             const std::string& dflt = {}) {
    if (!j.is_object() || !j.contains(key)) return dflt;
    const auto& v = j[key];
    return v.is_string() ? v.get<std::string>() : dflt;
}

// True/false, or the default — `is_boolean()` only, so `"true"` and `1` are
// rejected rather than silently reinterpreted.
inline bool readBool(const nlohmann::json& j, const char* key, bool dflt) {
    if (!j.is_object() || !j.contains(key)) return dflt;
    const auto& v = j[key];
    return v.is_boolean() ? v.get<bool>() : dflt;
}

} // namespace jsonread
