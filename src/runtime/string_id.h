#pragma once
#include <cstdint>
#include <string>

// ── FNV-1a 32-bit compile-time hash ───────────────────────────────────────
constexpr uint32_t fnv1a32(const char* s, uint32_t h = 2166136261u) noexcept {
    return *s ? fnv1a32(s + 1, (h ^ (uint8_t)*s) * 16777619u) : h;
}

// ── StringID ───────────────────────────────────────────────────────────────
// Pre-hashed string identifier. Implicit construction from const char* and
// std::string means all existing call sites compile unchanged:
//
//   Input::getAxis("MoveForward")        ← hash at call site (constexpr context)
//   static constexpr auto kMove = "MoveForward"_sid;  ← explicit compile-time
//
// Replaces std::string keys in InputMap — O(1) uint32 lookup vs O(n) string hash.
struct StringID {
    uint32_t id = 0;

    constexpr StringID() noexcept = default;
    constexpr StringID(const char* s)    noexcept : id(fnv1a32(s)) {}
    explicit  StringID(const std::string& s) noexcept : id(fnv1a32(s.c_str())) {}

    constexpr bool operator==(StringID o) const noexcept { return id == o.id; }
    constexpr bool operator!=(StringID o) const noexcept { return id != o.id; }
};

// "Jump"_sid — zero-cost compile-time literal
constexpr StringID operator""_sid(const char* s, size_t) noexcept {
    return StringID{s};
}

// std::unordered_map support
template<> struct std::hash<StringID> {
    size_t operator()(StringID s) const noexcept { return s.id; }
};
