#include "assetlib/uuid.h"
#include <random>


namespace assetlib {

static std::mt19937_64& rng() {
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    return gen;
}

UUID UUID::generate() {
    UUID u;
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t hi = dist(rng());
    uint64_t lo = dist(rng());

    // version 4
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    // variant bits
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    for (int i = 0; i < 8; ++i) u.bytes[i]   = (hi >> (56 - i*8)) & 0xFF;
    for (int i = 0; i < 8; ++i) u.bytes[8+i] = (lo >> (56 - i*8)) & 0xFF;
    return u;
}

UUID UUID::null() {
    UUID u; u.bytes.fill(0); return u;
}

bool UUID::isNull() const {
    for (auto b : bytes) if (b) return false;
    return true;
}

std::string UUID::toString() const {
    // xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    char buf[37];
    snprintf(buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0],bytes[1],bytes[2],bytes[3],
        bytes[4],bytes[5],bytes[6],bytes[7],
        bytes[8],bytes[9],bytes[10],bytes[11],
        bytes[12],bytes[13],bytes[14],bytes[15]);
    return buf;
}

UUID UUID::fromString(const std::string& s) {
    UUID u;
    std::string clean;
    clean.reserve(32);
    for (char c : s) if (c != '-') clean += c;
    if (clean.size() != 32) return UUID::null();
    for (int i = 0; i < 16; ++i) {
        unsigned val;
        if (sscanf(clean.c_str() + i*2, "%02x", &val) != 1) return UUID::null();
        u.bytes[i] = static_cast<uint8_t>(val);
    }
    return u;
}

} // namespace assetlib

size_t std::hash<assetlib::UUID>::operator()(const assetlib::UUID& u) const noexcept {
    size_t h = 0;
    for (auto b : u.bytes) h = h * 31 + b;
    return h;
}
