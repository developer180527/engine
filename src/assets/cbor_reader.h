#pragma once
// ── cbor_reader — CBOR read in place, without building a tree (WO-092) ───────
//
// The cooked scene's payload is CBOR (cooked_scene.h). Decoding it into
// nlohmann's JSON tree cost the shipped player most of a scene load: one
// allocation per node, built and freed for every entity. This reads the same
// bytes in place: a cursor over the buffer, items read one head at a time, a
// value skipped without being decoded, and nothing allocated.
//
// THE SUBSET. Exactly what nlohmann::json::to_cbor writes: unsigned and negative
// integers (the negative ones within int64), half, single and double floats,
// definite-length text strings, arrays and maps, true, false and null. Anything
// else (indefinite lengths, byte strings, tags, other simple values) is outside
// it, and validate() says so; the caller then takes the tree path, which reads
// such a file exactly as it always did. Map keys must be text, as nlohmann
// requires.
//
// UNTRUSTED. validate() walks the whole buffer once, bounds-checking every head
// and length against the bytes that exist and capping nesting at kMaxDepth, so a
// buffer it accepts can be read with the cursor without further checks failing.
// The reads check bounds anyway; a failed read returns false.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace cbor {

inline constexpr std::size_t kMaxDepth = 64;   // as cooked_scene::kMaxDepth

enum class Kind : uint8_t { UInt, NegInt, Float, Text, Array, Map, Bool, Null };

// One item's head. For Text, `text` is the string's bytes; for Array and Map,
// `count` is the number of elements (pairs for a map), which follow.
struct Item {
    Kind             kind = Kind::Null;
    uint64_t         u = 0;       // UInt
    int64_t          i = 0;       // NegInt, as nlohmann stores it
    double           d = 0.0;     // Float
    bool             b = false;   // Bool
    std::string_view text;        // Text
    uint64_t         count = 0;   // Array, Map
};

class Reader {
public:
    Reader(const uint8_t* data, std::size_t size) : m_p(data), m_n(size) {}

    std::size_t pos() const { return m_pos; }
    void        seek(std::size_t p) { m_pos = p; }
    const uint8_t* data() const { return m_p; }
    // Sticky: true once any read has failed. A reader used without validate()
    // first (the scene load, WO-092) checks it once at the end and rolls back.
    bool failed() const { return m_failed; }

    // Reads one head (and a text string's bytes) at the cursor and moves past
    // it. False on anything outside the subset or past the end, and on a
    // container whose count the remaining bytes cannot hold (each element is at
    // least one byte), so a loop over `count` cannot run long on a short buffer.
    bool head(Item& it) {
        if (headImpl(it)) return true;
        m_failed = true;
        return false;
    }

private:
    bool headImpl(Item& it) {
        if (m_pos >= m_n) return false;
        const uint8_t ib = m_p[m_pos++];
        const uint8_t major = ib >> 5, info = ib & 0x1F;
        if (major == 7) {
            switch (ib) {
                case 0xF4: it.kind = Kind::Bool; it.b = false; return true;
                case 0xF5: it.kind = Kind::Bool; it.b = true;  return true;
                case 0xF6: it.kind = Kind::Null; return true;
                case 0xF9: { uint64_t h; if (!be(2, h)) return false;
                             it.kind = Kind::Float; it.d = half((uint16_t)h); return true; }
                case 0xFA: { uint64_t f; if (!be(4, f)) return false;
                             float v; uint32_t w = (uint32_t)f; std::memcpy(&v, &w, 4);
                             it.kind = Kind::Float; it.d = v; return true; }
                case 0xFB: { uint64_t f; if (!be(8, f)) return false;
                             double v; std::memcpy(&v, &f, 8);
                             it.kind = Kind::Float; it.d = v; return true; }
                default: return false;   // other simple values: not written by nlohmann
            }
        }
        uint64_t arg;
        if (info < 24) arg = info;
        else if (info == 24) { if (!be(1, arg)) return false; }
        else if (info == 25) { if (!be(2, arg)) return false; }
        else if (info == 26) { if (!be(4, arg)) return false; }
        else if (info == 27) { if (!be(8, arg)) return false; }
        else return false;   // indefinite length or reserved
        switch (major) {
            case 0: it.kind = Kind::UInt; it.u = arg; return true;
            case 1:
                // nlohmann computes -1 - n in int64; outside int64 that overflows,
                // so such a value is outside the subset.
                if (arg > (uint64_t)INT64_MAX) return false;
                it.kind = Kind::NegInt; it.i = (int64_t)-1 - (int64_t)arg; return true;
            case 3:
                if (arg > m_n - m_pos) return false;
                it.kind = Kind::Text;
                it.text = std::string_view(reinterpret_cast<const char*>(m_p + m_pos), (std::size_t)arg);
                m_pos += (std::size_t)arg;
                return true;
            case 4:
                if (arg > m_n - m_pos) return false;
                it.kind = Kind::Array; it.count = arg; return true;
            case 5:
                if (arg > (m_n - m_pos) / 2) return false;
                it.kind = Kind::Map; it.count = arg; return true;
            default: return false;   // byte strings (2) and tags (6)
        }
    }

public:

    // Moves past one whole item. False if it is outside the subset, runs past
    // the end, or nests deeper than kMaxDepth.
    bool skip(std::size_t depth = 0) {
        Item it;
        if (!head(it)) return false;
        if (it.kind != Kind::Array && it.kind != Kind::Map) return true;
        if (depth + 1 > kMaxDepth) { m_failed = true; return false; }
        for (uint64_t k = 0; k < it.count; ++k) {
            if (it.kind == Kind::Map) {
                Item key;
                if (!head(key)) return false;
                if (key.kind != Kind::Text) { m_failed = true; return false; }
            }
            if (!skip(depth + 1)) return false;
        }
        return true;
    }

private:
    bool be(int bytes, uint64_t& out) {
        if ((std::size_t)bytes > m_n - m_pos) return false;
        const uint8_t* q = m_p + m_pos;
        switch (bytes) {
            case 1: out = q[0]; break;
            case 2: out = ((uint64_t)q[0] << 8) | q[1]; break;
            case 4: out = ((uint64_t)q[0] << 24) | ((uint64_t)q[1] << 16)
                        | ((uint64_t)q[2] << 8) | q[3]; break;
            default:
                out = 0;
                for (int k = 0; k < bytes; ++k) out = (out << 8) | q[k];
        }
        m_pos += (std::size_t)bytes;
        return true;
    }
    // IEEE 754 half to double, as nlohmann decodes it.
    static double half(uint16_t h) {
        const int exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
        double val;
        if (exp == 0)       val = std::ldexp((double)mant, -24);
        else if (exp != 31) val = std::ldexp((double)(mant + 1024), exp - 25);
        else                val = mant == 0 ? INFINITY : NAN;
        return (h & 0x8000) ? -val : val;
    }

    const uint8_t* m_p;
    std::size_t    m_n;
    std::size_t    m_pos = 0;
    bool           m_failed = false;
};

// The whole buffer is exactly one item in the subset, no deeper than kMaxDepth.
inline bool validate(const uint8_t* data, std::size_t size) {
    Reader r(data, size);
    return r.skip() && r.pos() == size;
}

// ── What nlohmann's tree would answer, read off one item ────────────────────
// The scene readers ask a value a handful of questions (is it a number, a
// boolean, a string; what is it as a float, an integer, an unsigned). These
// give the answers nlohmann::json gives for the same bytes, conversion casts
// included, so a field reads the same through either path.
inline bool isNumber(const Item& v) {
    return v.kind == Kind::UInt || v.kind == Kind::NegInt || v.kind == Kind::Float;
}
template <class T>
inline T numberAs(const Item& v) {   // nlohmann's get<T>() on a number
    switch (v.kind) {
        case Kind::UInt:   return static_cast<T>(v.u);
        case Kind::NegInt: return static_cast<T>(v.i);
        default:           return static_cast<T>(v.d);
    }
}

}  // namespace cbor
