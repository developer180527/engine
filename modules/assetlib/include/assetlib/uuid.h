#pragma once
#include <array>
#include <string>
#include <cstdint>
#include <functional> // std::hash

namespace assetlib {

// 128-bit UUID — stable across runs, stored as hex string in the registry.
// Format: "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx" (UUID v4 layout)
struct UUID {
    std::array<uint8_t, 16> bytes{};

    bool operator==(const UUID& o) const { return bytes == o.bytes; }
    bool operator!=(const UUID& o) const { return bytes != o.bytes; }
    bool operator< (const UUID& o) const { return bytes <  o.bytes; }

    bool        isNull()   const;
    std::string toString() const;          // "a3f7c2d1-e5b8-4f9a-b2c3-..."
    static UUID fromString(const std::string& s);
    static UUID generate();                // cryptographically random
    static UUID null();                    // all-zeros sentinel
};

} // namespace assetlib

// std::unordered_map support
template<> struct std::hash<assetlib::UUID> {
    size_t operator()(const assetlib::UUID& u) const noexcept;
};
