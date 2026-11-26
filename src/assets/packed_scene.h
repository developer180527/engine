#pragma once
// ── packed_scene — the cooked scene's packed payload (WO-092) ────────────────
//
// The fastest form of a cooked scene: each registered component the cook could
// pack is written by its schema, as fixed binary fields, so the shipped player
// copies them into the component with no parsing at all. What cannot be packed
// (a component with a codec of its own, an unknown key, a kit's block, data at
// another revision) rides along as CBOR, which the player reads in place
// (cbor_reader.h), as it reads a CBOR payload.
//
// The cooker cannot pack: the schemas are the scene layer's (src/scene), which
// depends on this module. So the cooker takes a packer function from whoever
// owns the cook (cooked_scene::Packer); the tools that cook install the scene
// layer's (EntitySerde::scenePacker()). A cook without one writes CBOR, which is
// correct and slower to load.
//
// This header knows the LAYOUT, not the schemas: it reads the schema table, and
// walks entities and their records, so the packager and a preload can find the
// meshes a scene names without the scene layer.
//
//   u32 magic "SPKD", u32 layout (1)
//   u32 schemaCount; per schema: str name, str key, u32 revision,
//       u32 fieldCount; per field: str name, u8 type (EntitySerde::FieldType), u32 count
//   u32 entityCount; per entity:
//       u64 wantedId   the id the JSON reader would take (idFrom), 0: none
//       u64 fileId     the id as parent links read it (u64From), 0: none
//       u64 parentId   u64From, 0: none
//       u32 recordCount; per record: u32 schema (index into the table), u32 size, bytes
//       u32 restSize; bytes: a CBOR map of everything not packed, or nothing
//   str: u32 size, bytes. All little-endian.
//
// UNTRUSTED. Every count and size is checked against the bytes that remain
// before it is used, and a read past the end fails the reader (sticky), so a
// corrupt file is refused, never over-read.
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace packed {

// std::endian, not __BYTE_ORDER__: MSVC defines neither of the GCC/Clang macros,
// so that form did not compile on Windows.
static_assert(std::endian::native == std::endian::little,
              "the packed scene layout is little-endian and written as memory");

inline constexpr uint32_t kMagic  = 0x444B5053;   // "SPKD"
inline constexpr uint32_t kLayout = 1;

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : m_b(out) {}
    void u8(uint8_t v)   { m_b.push_back(v); }
    void u32(uint32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void raw(const void* p, std::size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        m_b.insert(m_b.end(), b, b + n);
    }
    void str(std::string_view s) { u32((uint32_t)s.size()); raw(s.data(), s.size()); }
    std::size_t size() const { return m_b.size(); }
    // A size written now and filled in later (a record's length).
    std::size_t reserve32() { const std::size_t at = m_b.size(); u32(0); return at; }
    void patch32(std::size_t at, uint32_t v) { std::memcpy(m_b.data() + at, &v, 4); }
private:
    std::vector<uint8_t>& m_b;
};

class Reader {
public:
    Reader(const uint8_t* p, std::size_t n) : m_p(p), m_n(n) {}
    bool ok() const { return m_ok; }
    bool atEnd() const { return m_pos == m_n; }
    std::size_t remaining() const { return m_n - m_pos; }
    std::size_t pos() const { return m_pos; }
    const uint8_t* here() const { return m_p + m_pos; }

    bool raw(void* out, std::size_t n) {
        if (!m_ok || n > m_n - m_pos) { m_ok = false; return false; }
        std::memcpy(out, m_p + m_pos, n);
        m_pos += n;
        return true;
    }
    uint8_t  u8()  { uint8_t v = 0;  raw(&v, 1); return v; }
    uint32_t u32() { uint32_t v = 0; raw(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; raw(&v, 8); return v; }
    std::string_view str() {
        const uint32_t n = u32();
        if (!m_ok || n > m_n - m_pos) { m_ok = false; return {}; }
        std::string_view s(reinterpret_cast<const char*>(m_p + m_pos), n);
        m_pos += n;
        return s;
    }
    // `n` bytes in place.
    const uint8_t* bytes(std::size_t n) {
        if (!m_ok || n > m_n - m_pos) { m_ok = false; return nullptr; }
        const uint8_t* p = m_p + m_pos;
        m_pos += n;
        return p;
    }
    void fail() { m_ok = false; }
private:
    const uint8_t* m_p;
    std::size_t    m_n;
    std::size_t    m_pos = 0;
    bool           m_ok = true;
};

struct Field  { std::string name; uint8_t type = 0; uint32_t count = 0; };
struct Schema { std::string name, key; uint32_t revision = 0; std::vector<Field> fields; };

struct Record { uint32_t schema = 0; const uint8_t* data = nullptr; uint32_t size = 0; };
struct Entity {
    uint64_t wantedId = 0, fileId = 0, parentId = 0;
    std::vector<Record> records;
    const uint8_t* rest = nullptr;   // CBOR map, or null
    uint32_t restSize = 0;
};

// The payload, walked: open() reads the schema table, next() each entity.
class View {
public:
    bool open(const uint8_t* data, std::size_t size, std::string& why) {
        m_r = Reader(data, size);
        if (m_r.u32() != kMagic || !m_r.ok()) { why = "not a packed scene"; return false; }
        const uint32_t layout = m_r.u32();
        if (layout != kLayout) {
            why = "packed scene layout " + std::to_string(layout) + ", this engine reads "
                + std::to_string(kLayout) + " (re-cook the scene)";
            return false;
        }
        const uint32_t ns = m_r.u32();
        if (!m_r.ok() || ns > m_r.remaining() / 16) { why = "schema table larger than the file"; return false; }
        m_schemas.resize(ns);
        for (Schema& s : m_schemas) {
            s.name = std::string(m_r.str());
            s.key  = std::string(m_r.str());
            s.revision = m_r.u32();
            const uint32_t nf = m_r.u32();
            if (!m_r.ok() || nf > m_r.remaining() / 9) { why = "schema table truncated"; return false; }
            s.fields.resize(nf);
            for (Field& f : s.fields) { f.name = std::string(m_r.str()); f.type = m_r.u8(); f.count = m_r.u32(); }
        }
        m_count = m_r.u32();
        if (!m_r.ok() || m_count > m_r.remaining() / 32) { why = "entity count larger than the file"; return false; }
        m_left = m_count;
        return true;
    }
    const std::vector<Schema>& schemas() const { return m_schemas; }
    uint32_t entityCount() const { return m_count; }

    // The next entity, or false at the end or on a malformed one (`failed()`).
    bool next(Entity& e) {
        if (m_left == 0 || !m_r.ok()) return false;
        --m_left;
        e.wantedId = m_r.u64(); e.fileId = m_r.u64(); e.parentId = m_r.u64();
        const uint32_t nr = m_r.u32();
        if (!m_r.ok() || nr > m_r.remaining() / 8) { m_r.fail(); return false; }
        e.records.resize(nr);
        for (Record& rec : e.records) {
            rec.schema = m_r.u32();
            rec.size   = m_r.u32();
            rec.data   = m_r.bytes(rec.size);
            if (!m_r.ok() || rec.schema >= m_schemas.size()) { m_r.fail(); return false; }
        }
        e.restSize = m_r.u32();
        e.rest = e.restSize ? m_r.bytes(e.restSize) : nullptr;
        if (!m_r.ok()) return false;
        return true;
    }
    bool failed() const { return !m_r.ok(); }
    // Every entity read and nothing after them.
    bool complete() const { return m_r.ok() && m_left == 0 && m_r.atEnd(); }

private:
    Reader              m_r{nullptr, 0};
    std::vector<Schema> m_schemas;
    uint32_t            m_count = 0, m_left = 0;
};

}  // namespace packed
