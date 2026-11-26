#pragma once
// ── Component schemas: what a saved component IS (WO-071) ────────────────────
//
// One registry describes every component the scene format knows: its namespaced
// name (`core.Camera`), the key it is saved under (`"camera"`), its revision, and
// how its data is read and written. The scene save, the in-memory snapshot that
// Play and undo use, and the undo stack all walk this registry, so a component
// registered once is saved, snapshotted and undone everywhere.
//
// MOST COMPONENTS ARE FIELDS. A component that is plain data is described field
// by field (`SchemaBuilder<T>`), and one generic reader and writer handles all of
// them. A component whose saved form is not its in-memory form (a mesh saved as
// an asset reference, loaded through the asset system) registers its own codec
// instead; it still has a name, a key and a revision like every other.
//
// REVISIONS AND MIGRATIONS (DR-0034). A schema's revision is its component's
// `kAbiRevision` (WO-051). Data saved under revision 0 carries no marker, which
// is every scene saved before this existed; a later revision writes `"$rev"`
// inside the component's object. Loading older data runs the schema's
// migrations in order, one revision at a time. Data that cannot be read here
// (newer than this engine, or a revision with no migration) is NOT dropped: it is
// kept as an unknown component and saved back unchanged, with one warning that
// names it (DR-0035).
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "assets/asset_ref.h"
#include "assets/cbor_reader.h"
#include "assets/packed_scene.h"
#include "core/json_read.h"
#include "core/logger.h"

namespace EntitySerde {

struct SerdeContext;   // entity_serializer.h: mode, asset hooks, id index

// Entity references, defined in entity_serializer.h where the context is whole.
// A field naming another entity saves that entity's stable id (EntityId), and on
// load is set once every entity of the scene exists (resolveEntityRefs).
uint64_t stableIdOf(flecs::entity target);
void deferEntityRef(SerdeContext& ctx, flecs::entity owner, uint64_t stableId,
                    std::function<void(flecs::entity owner, flecs::entity target)> apply);
bool beginEntityRefs(SerdeContext& ctx);
void endEntityRefs(SerdeContext& ctx, flecs::world w, bool owned);
// The context a cook packs with: disk mode, and a reference list nothing reads.
SerdeContext& packContext();

// What a field holds, as the engine API's draft names its field types
// (docs/plans/engine-api-draft/eng/eng_world.h, EngFieldType). An enum is saved as
// its integer value and described as I32. Variable-length blobs are WO-073's.
enum class FieldType : uint8_t {
    Bool, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64,
    Vec2, Vec3, Vec4, Quat, Mat4, String, Entity, Asset,
};
inline const char* fieldTypeName(FieldType t) {
    static const char* k[] = { "bool", "i8", "u8", "i16", "u16", "i32", "u32", "i64",
        "u64", "f32", "f64", "vec2", "vec3", "vec4", "quat", "mat4", "string",
        "entity", "asset" };
    return k[(int)t];
}
// The bytes a field of this type takes in a packed record (0: variable or a
// composite, written by its own rule: string, entity, asset).
inline constexpr std::size_t packedSize(FieldType t) {
    switch (t) {
        case FieldType::Bool: case FieldType::I8: case FieldType::U8: return 1;
        case FieldType::I16: case FieldType::U16: return 2;
        case FieldType::I32: case FieldType::U32: case FieldType::F32: return 4;
        case FieldType::I64: case FieldType::U64: case FieldType::F64: return 8;
        case FieldType::Vec2: return 8;
        case FieldType::Vec3: return 12;
        case FieldType::Vec4: case FieldType::Quat: return 16;
        case FieldType::Mat4: return 64;
        default: return 0;
    }
}

struct FieldInfo {
    std::string name;    // the key inside the component's object
    FieldType   type;
    uint32_t    count;   // 1, or N for an array of N f32 (floats<N>)
};

// The key a revision is saved under, inside the component's object. `$` keeps it
// apart from any field name a component could have.
inline constexpr const char* kRevisionKey = "$rev";

struct ComponentSchema {
    std::string name;          // namespaced identity: "core.Camera", "apollo.Thruster"
    std::string key;           // the entity object's key on disk: "camera"
    uint32_t    revision = 0;  // the component's kAbiRevision

    std::function<bool(flecs::entity)> has;
    // Writes the component's value into `out` (an object, or for a few legacy
    // components a bare value such as Name's string).
    std::function<void(flecs::entity, nlohmann::json& out, const SerdeContext&)> save;
    // Reads `in`, already migrated to `revision`, and sets the component. Throws
    // on data it cannot read at all; the caller skips the component and says so.
    std::function<void(flecs::entity, const nlohmann::json& in, SerdeContext&)> load;
    std::function<void(flecs::entity)> remove;

    // The same read, straight from the cooked scene's CBOR at `pos` (a map),
    // with no JSON tree built (WO-092). Set for field-by-field schemas and for
    // the few codecs that have one. Returns false, having done nothing, when the
    // data needs the JSON path: not a map, or carrying a revision marker. The
    // caller then builds the value as JSON and calls `load`.
    // On success `*end` is where the value ended, so the caller need not walk it
    // again.
    std::function<bool(flecs::entity, const uint8_t* data, std::size_t size, std::size_t pos,
                       SerdeContext&, std::size_t* end)> loadCbor;
    // ── Packed (WO-092, assets/packed_scene.h) ─────────────────────────────
    // `pack` writes a value (already at this revision) as `packedFields`, by
    // reading it into the component with the same field readers `load` uses and
    // writing the component's fields; false when the value cannot be packed (not
    // an object, for most). `unpack` reads them back and sets the component;
    // false on bytes that do not fit. Field-by-field schemas and Name have them.
    // `packedFields` is what the cooked file's schema table records: a field
    // named "" is the value itself (Name's string), not a key of an object.
    std::function<bool(const nlohmann::json& value, packed::Writer&)> pack;
    std::function<bool(flecs::entity, packed::Reader&, SerdeContext&)> unpack;
    std::vector<FieldInfo> packedFields;

    // Reads nothing but its own value, so it may load in whatever order the
    // keys come (the CBOR reader does it as it meets them). False for a codec
    // that depends on another component being loaded first (the LOD chain on
    // its mesh) or on the entity's later ones (a mesh's skin on its Animator).
    bool orderFree = false;

    // Field-by-field schemas only (empty for a codec of its own): every field
    // with its type, and the saved form of a default-constructed component,
    // which is what a field missing from saved data reads as.
    std::vector<FieldInfo> fields;
    nlohmann::json         defaults;

    // migrations[r] upgrades data saved under revision r to revision r + 1, in
    // place. There must be one for every revision below `revision` that saved
    // data can still have; a gap is reported when such data is loaded.
    std::vector<std::function<void(nlohmann::json&)>> migrations;
};

// The schemas, in load order: a component another one depends on comes first
// (the LOD chain after the mesh it belongs to).
class SchemaRegistry {
public:
    // Registering the same name, key and revision again is a no-op. Anything else
    // that collides (a key or name already taken by a different schema, or the
    // same component at another revision) is refused, naming both sides.
    bool add(ComponentSchema s) {
        for (const ComponentSchema& have : m_schemas) {
            if (have.name != s.name && have.key != s.key) continue;
            if (have.name == s.name && have.key == s.key && have.revision == s.revision)
                return true;
            LOG_ERROR("Schema", "refused \"%s\" (key \"%s\", revision %u): \"%s\" "
                      "(key \"%s\", revision %u) is already registered",
                      s.name.c_str(), s.key.c_str(), s.revision, have.name.c_str(),
                      have.key.c_str(), have.revision);
            return false;
        }
        if (s.revision > 0 && s.migrations.size() < s.revision)
            s.migrations.resize(s.revision);   // the gaps report themselves at load
        m_schemas.push_back(std::move(s));
        return true;
    }

    const ComponentSchema* byKey(std::string_view key) const {
        for (const ComponentSchema& s : m_schemas) if (s.key == key) return &s;
        return nullptr;
    }
    const ComponentSchema* byName(std::string_view name) const {
        for (const ComponentSchema& s : m_schemas) if (s.name == name) return &s;
        return nullptr;
    }
    const std::vector<ComponentSchema>& all() const { return m_schemas; }

private:
    std::vector<ComponentSchema> m_schemas;
};

// Brings `data`, saved under the revision it carries, to `schema.revision`.
// Returns false, with `why` set, when it cannot: newer than this engine, or a
// migration missing. On success the revision marker is removed.
inline bool migrateToCurrent(const ComponentSchema& schema, nlohmann::json& data,
                             std::string& why) {
    uint32_t rev = 0;
    if (data.is_object()) {
        if (auto it = data.find(kRevisionKey); it != data.end()) {
            if (!it->is_number_unsigned() || it->get<uint64_t>() > UINT32_MAX) {
                why = "its revision marker is not a revision number";
                return false;
            }
            rev = (uint32_t)it->get<uint64_t>();
        }
    }
    if (rev > schema.revision) {
        why = "it was saved at revision " + std::to_string(rev)
            + ", newer than this engine's " + std::to_string(schema.revision);
        return false;
    }
    for (uint32_t r = rev; r < schema.revision; ++r) {
        if (r >= schema.migrations.size() || !schema.migrations[r]) {
            why = "no migration from revision " + std::to_string(r) + " to "
                + std::to_string(r + 1);
            return false;
        }
        schema.migrations[r](data);
    }
    if (data.is_object()) data.erase(kRevisionKey);
    return true;
}

// ── One value, read the same from JSON or from CBOR ─────────────────────────
// Every field reader below is written ONCE, against this small interface, and
// instantiated for both: the editor reads a .scene as JSON, the shipped player
// reads the cooked scene's CBOR in place (WO-092). Two readers written apart
// would drift; one reader over two adapters cannot. The CBOR adapter gives the
// answers nlohmann's tree gives for the same bytes (cbor_reader.h), and
// fuzz_scene_reader_parity_test checks the two against each other.
struct JsonValue {
    const nlohmann::json* j;
    bool isNumber()   const { return j->is_number(); }
    bool isUnsigned() const { return j->is_number_unsigned(); }
    bool isInteger()  const { return j->is_number_integer(); }
    template <class N> N number() const { return j->template get<N>(); }
    bool isBool()     const { return j->is_boolean(); }
    bool asBool()     const { return j->template get<bool>(); }
    bool isString()   const { return j->is_string(); }
    std::string asString() const { return j->template get<std::string>(); }
    bool isArray()    const { return j->is_array(); }
    bool isObject()   const { return j->is_object(); }
    std::size_t size() const { return j->size(); }
    JsonValue at(std::size_t k) const { return { &(*j)[k] }; }
    // The first n elements, in order.
    template <class F> void elements(std::size_t n, F&& fn) const {
        for (std::size_t k = 0; k < n; ++k) fn(k, JsonValue{&(*j)[k]});
    }
    bool find(const char* key, JsonValue& out) const {
        if (!j->is_object()) return false;
        auto it = j->find(key);
        if (it == j->end()) return false;
        out = { &*it };
        return true;
    }
};

struct CborValue {
    const uint8_t* data = nullptr;
    std::size_t    len = 0;
    std::size_t    body = 0;    // just past this item's head
    cbor::Item     item;
    bool           ok = false;
    // Where the value ends, once known: at once for a scalar, and for a
    // container after elements() has walked all of it.
    mutable std::size_t endPos = SIZE_MAX;

    CborValue() = default;
    CborValue(const uint8_t* d, std::size_t n, std::size_t pos) : data(d), len(n) {
        cbor::Reader r(d, n);
        r.seek(pos);
        ok = r.head(item);
        body = r.pos();
        if (ok && item.kind != cbor::Kind::Array && item.kind != cbor::Kind::Map) endPos = body;
    }
    bool isNumber()   const { return ok && cbor::isNumber(item); }
    bool isUnsigned() const { return ok && item.kind == cbor::Kind::UInt; }
    bool isInteger()  const { return ok && (item.kind == cbor::Kind::UInt || item.kind == cbor::Kind::NegInt); }
    template <class N> N number() const { return cbor::numberAs<N>(item); }
    bool isBool()     const { return ok && item.kind == cbor::Kind::Bool; }
    bool asBool()     const { return item.b; }
    bool isString()   const { return ok && item.kind == cbor::Kind::Text; }
    std::string asString() const { return std::string(item.text); }
    bool isArray()    const { return ok && item.kind == cbor::Kind::Array; }
    bool isObject()   const { return ok && item.kind == cbor::Kind::Map; }
    std::size_t size() const { return (std::size_t)item.count; }
    CborValue at(std::size_t k) const {
        cbor::Reader r(data, len);
        r.seek(body);
        for (std::size_t n = 0; n < k; ++n) r.skip();
        return CborValue(data, len, r.pos());
    }
    // The first n elements, in order: one walk, not one per element.
    template <class F> void elements(std::size_t n, F&& fn) const {
        cbor::Reader r(data, len);
        r.seek(body);
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t at = r.pos();
            const CborValue e(data, len, at);
            fn(k, e);
            if (e.endPos != SIZE_MAX) r.seek(e.endPos);
            else { r.seek(at); if (!r.skip()) return; }
        }
        if (n == item.count && item.kind == cbor::Kind::Array) endPos = r.pos();
    }
    // The LAST pair with this key, as nlohmann's tree keeps it.
    bool find(const char* key, CborValue& out) const {
        if (!isObject()) return false;
        cbor::Reader r(data, len);
        r.seek(body);
        bool found = false;
        const std::string_view want(key);
        for (uint64_t n = 0; n < item.count; ++n) {
            cbor::Item k;
            if (!r.head(k)) return found;
            const std::size_t at = r.pos();
            if (k.text == want) { out = CborValue(data, len, at); found = true; }
            r.seek(at);
            if (!r.skip()) return found;
        }
        return found;
    }
};

// The stable id an entity was saved with, as `je.value("id", 0)` reads it: a
// number by nlohmann's cast, a boolean as 0 or 1, anything else none.
template <class V>
inline uint64_t idFrom(const V& v) {
    if (v.isNumber()) return v.template number<uint64_t>();
    if (v.isBool())   return v.asBool() ? 1 : 0;
    return 0;
}
// An unsigned id field as jsonread::readU64 reads it (parentId, entity fields).
template <class V>
inline uint64_t u64From(const V& v, uint64_t dflt = 0) {
    return v.isInteger() ? v.template number<uint64_t>() : dflt;
}

// ── Field-by-field schemas ──────────────────────────────────────────────────
// Reads are tolerant per field, the rule every scene reader here follows: a
// field that is missing, of the wrong type, out of range or not finite keeps the
// component's default, and the rest of the component still loads. Data that is
// not an object at all is refused, so the component is skipped as before.
template <class T>
class SchemaBuilder {
    // A field's reader: given the value saved under its key (present), update t.
    // A generic lambda, instantiated for JsonValue and for CborValue.
#define ENGINE_FIELD_READ [key, m](T& t, const auto& v, flecs::entity owner, SerdeContext& ctx)
public:
    SchemaBuilder(std::string name, std::string key) {
        m_s.name = std::move(name);
        m_s.key  = std::move(key);
        m_s.revision = revisionOf();
        m_s.has    = [](flecs::entity e) { return e.try_get<T>() != nullptr; };
        m_s.remove = [](flecs::entity e) { e.remove<T>(); };
    }

    SchemaBuilder& boolean(const char* key, bool T::*m) {
        return add(key, FieldType::Bool, 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity) { j[key] = t.*m; },
            ENGINE_FIELD_READ { (void)owner; (void)ctx; if (v.isBool()) t.*m = v.asBool(); },
            [m](const T& t, const nlohmann::json*, packed::Writer& w) { w.u8(t.*m ? 1 : 0); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) { t.*m = r.u8() != 0; });
    }
    // Any integer type, read only when the saved value is an integer that fits.
    // nlohmann keeps a non-negative integer as unsigned and a negative one as
    // signed, so both are read, and the value is range-checked against I.
    template <class I>
    SchemaBuilder& integer(const char* key, I T::*m) {
        static_assert(std::is_integral_v<I> && !std::is_same_v<I, bool>);
        static_assert(sizeof(I) == packedSize(integerType<I>()),
                      "an integer field's type must be a fixed-width integer");
        return add(key, integerType<I>(), 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity) { j[key] = t.*m; },
            ENGINE_FIELD_READ {
                (void)owner; (void)ctx;
                if (v.isUnsigned()) {
                    const uint64_t u = v.template number<uint64_t>();
                    if (u <= (uint64_t)std::numeric_limits<I>::max()) t.*m = (I)u;
                } else if (v.isInteger()) {
                    const int64_t s = v.template number<int64_t>();
                    if constexpr (std::is_signed_v<I>) {
                        if (s >= (int64_t)std::numeric_limits<I>::min()
                            && s <= (int64_t)std::numeric_limits<I>::max()) t.*m = (I)s;
                    } else {
                        if (s >= 0 && (uint64_t)s <= (uint64_t)std::numeric_limits<I>::max()) t.*m = (I)s;
                    }
                } },
            [m](const T& t, const nlohmann::json*, packed::Writer& w) { const I v = t.*m; w.raw(&v, sizeof(I)); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) { r.raw(&(t.*m), sizeof(I)); });
    }
    SchemaBuilder& u8(const char* key, uint8_t T::*m) { return integer(key, m); }
    // An enum saved as its integer value. A value outside the enum is kept as
    // read: whoever uses it refuses it, so a scene from a newer engine round-trips.
    template <class E>
    SchemaBuilder& enumeration(const char* key, E T::*m) {
        static_assert(std::is_enum_v<E>);
        return add(key, FieldType::I32, 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity) { j[key] = (int)(t.*m); },
            ENGINE_FIELD_READ { (void)owner; (void)ctx; if (v.isNumber()) t.*m = (E)v.template number<int>(); },
            // As an int32, the integer JSON saves: (int)(E), and back as (E)(int).
            [m](const T& t, const nlohmann::json*, packed::Writer& w) {
                const int32_t v = (int32_t)(int)(t.*m); w.raw(&v, 4); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) {
                int32_t v = 0; r.raw(&v, 4); t.*m = (E)(int)v; });
    }
    SchemaBuilder& f32(const char* key, float T::*m) {
        return add(key, FieldType::F32, 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity) { j[key] = t.*m; },
            ENGINE_FIELD_READ {
                (void)owner; (void)ctx;
                if (!v.isNumber()) return;
                const float f = v.template number<float>();
                if (std::isfinite(f)) t.*m = f; },
            [m](const T& t, const nlohmann::json*, packed::Writer& w) { w.raw(&(t.*m), 4); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) { r.raw(&(t.*m), 4); });
    }
    SchemaBuilder& f64(const char* key, double T::*m) {
        return add(key, FieldType::F64, 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity) { j[key] = t.*m; },
            ENGINE_FIELD_READ {
                (void)owner; (void)ctx;
                if (!v.isNumber()) return;
                const double d = v.template number<double>();
                if (std::isfinite(d)) t.*m = d; },
            [m](const T& t, const nlohmann::json*, packed::Writer& w) { w.raw(&(t.*m), 8); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) { r.raw(&(t.*m), 8); });
    }
    // Vectors, a quaternion and a matrix: N floats saved as an array, from a
    // float[N] or a type that is exactly N floats (bx::Vec3, bx::Quaternion).
    template <class M> SchemaBuilder& vec2(const char* k, M T::*m) { return floatArray<2>(k, m, FieldType::Vec2); }
    template <class M> SchemaBuilder& vec3(const char* k, M T::*m) { return floatArray<3>(k, m, FieldType::Vec3); }
    template <class M> SchemaBuilder& vec4(const char* k, M T::*m) { return floatArray<4>(k, m, FieldType::Vec4); }
    template <class M> SchemaBuilder& quat(const char* k, M T::*m) { return floatArray<4>(k, m, FieldType::Quat); }
    // 16 floats, column-major, as the engine API's draft states its MAT4.
    template <class M> SchemaBuilder& mat4(const char* k, M T::*m) { return floatArray<16>(k, m, FieldType::Mat4); }
    // Any other fixed count of floats.
    template <std::size_t N, class M>
    SchemaBuilder& floats(const char* k, M T::*m) { return floatArray<N>(k, m, FieldType::F32); }

    // A string; `omitEmpty` leaves an empty one out of the saved object.
    SchemaBuilder& string(const char* key, std::string T::*m, bool omitEmpty = false) {
        return add(key, FieldType::String, 1,
            [key, m, omitEmpty](const T& t, nlohmann::json& j, flecs::entity) {
                if (!omitEmpty || !(t.*m).empty()) j[key] = t.*m; },
            ENGINE_FIELD_READ { (void)owner; (void)ctx; if (v.isString()) t.*m = v.asString(); },
            [m](const T& t, const nlohmann::json*, packed::Writer& w) { w.str(t.*m); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) { t.*m = std::string(r.str()); });
    }
    // Another entity. In memory a flecs id; saved as that entity's stable id (0:
    // none), and set on load once the entity it names exists. A reference to an
    // entity that is not in the scene loads as none, with a warning.
    SchemaBuilder& entity(const char* key, flecs::entity_t T::*m) {
        return add(key, FieldType::Entity, 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity owner) {
                j[key] = t.*m && owner.is_valid()
                       ? stableIdOf(flecs::entity(owner.world(), t.*m)) : (uint64_t)0; },
            ENGINE_FIELD_READ {
                t.*m = 0;   // never a flecs id from another world or session
                const uint64_t id = u64From(v);
                if (!id) return;
                deferEntityRef(ctx, owner, id, [m](flecs::entity o, flecs::entity target) {
                    if (T* c = o.try_get_mut<T>()) { c->*m = target.id(); o.modified<T>(); }
                }); },
            // Whether the key was saved, then the stable id it names: absent keeps
            // the default, present clears it and links once the entity exists.
            [](const T&, const nlohmann::json* v, packed::Writer& w) {
                w.u8(v ? 1 : 0);
                w.u64(v ? u64From(JsonValue{v}) : 0); },
            [m](T& t, packed::Reader& r, flecs::entity owner, SerdeContext& ctx) {
                const bool present = r.u8() != 0;
                const uint64_t id = r.u64();
                if (!present) return;
                t.*m = 0;
                if (!id) return;
                deferEntityRef(ctx, owner, id, [m](flecs::entity o, flecs::entity target) {
                    if (T* c = o.try_get_mut<T>()) { c->*m = target.id(); o.modified<T>(); }
                }); });
    }
    // An asset, by identity: its UUID and project-relative path (AssetRef), what
    // the component means rather than whatever loaded it. Saved as
    // {"asset": uuid, "path": path}, the shape every asset reference here has.
    SchemaBuilder& asset(const char* key, AssetRef T::*m) {
        return add(key, FieldType::Asset, 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity) {
                nlohmann::json a = nlohmann::json::object();
                assetref::toJson(t.*m, a);
                j[key] = std::move(a); },
            ENGINE_FIELD_READ {
                (void)owner; (void)ctx;
                if (!v.isObject()) return;
                std::decay_t<decltype(v)> s;
                (t.*m).uuid = v.find("asset", s) && s.isString() ? s.asString() : std::string{};
                (t.*m).path = v.find("path", s)  && s.isString() ? s.asString() : std::string{}; },
            [m](const T& t, const nlohmann::json*, packed::Writer& w) { w.str((t.*m).uuid); w.str((t.*m).path); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) {
                (t.*m).uuid = std::string(r.str()); (t.*m).path = std::string(r.str()); });
    }

    // Saved only while `pred` holds (a script with no path is not saved).
    SchemaBuilder& savedWhen(bool (*pred)(const T&)) {
        m_s.has = [pred](flecs::entity e) { const T* t = e.try_get<T>(); return t && pred(*t); };
        return *this;
    }
    SchemaBuilder& migration(uint32_t fromRevision, std::function<void(nlohmann::json&)> fn) {
        if (m_s.migrations.size() <= fromRevision) m_s.migrations.resize(fromRevision + 1);
        m_s.migrations[fromRevision] = std::move(fn);
        return *this;
    }
    // For a test's component, whose revision is the point of the test.
    SchemaBuilder& revision(uint32_t r) { m_s.revision = r; return *this; }
    // What a field missing from saved data reads as, when that is not the
    // struct's own default: the scene format's defaults are a promise to every
    // file already written (a rigid body with no "bodyType" has always been
    // kinematic, though a RigidBody constructed in code is dynamic).
    SchemaBuilder& defaults(const T& d) { m_defaults = std::make_shared<const T>(d); return *this; }

    ComponentSchema build() {
        auto fields = std::make_shared<const std::vector<Field>>(std::move(m_fields));
        std::shared_ptr<const T> base = m_defaults ? m_defaults : std::make_shared<const T>();
        {   // The defaults, as saved: what a component with no saved fields reads as.
            nlohmann::json d = nlohmann::json::object();
            for (const Field& f : *fields) f.save(*base, d, flecs::entity());
            m_s.defaults = std::move(d);
        }
        m_s.save = [fields](flecs::entity e, nlohmann::json& j, const SerdeContext&) {
            const T* t = e.try_get<T>(); if (!t) return;
            j = nlohmann::json::object();
            for (const Field& f : *fields) f.save(*t, j, e);
        };
        m_s.load = [fields, base](flecs::entity e, const nlohmann::json& j, SerdeContext& ctx) {
            if (!j.is_object()) throw std::runtime_error("not an object");
            T t = *base;
            const bool ownRefs = beginEntityRefs(ctx);   // see entity()
            try {
                for (const Field& f : *fields) {
                    auto it = j.find(f.key);
                    if (it != j.end()) f.loadJson(t, JsonValue{&*it}, e, ctx);
                }
                e.set<T>(std::move(t));
            } catch (...) { endEntityRefs(ctx, e.world(), ownRefs); throw; }
            endEntityRefs(ctx, e.world(), ownRefs);
        };
        m_s.loadCbor = [fields, base](flecs::entity e, const uint8_t* data, std::size_t size,
                                      std::size_t pos, SerdeContext& ctx, std::size_t* end) {
            const CborValue obj(data, size, pos);
            if (!obj.isObject()) return false;
            // One walk: each field read as its key comes. A repeated key, or a
            // revision marker, hands the whole component to the JSON path (the
            // tree keeps a repeated key's last value; a marker means migrate),
            // and nothing read here is kept: `t` is set only at the end.
            const std::size_t nf = fields->size();
            uint64_t seenBits = 0;
            T t = *base;
            const bool ownRefs = beginEntityRefs(ctx);
            struct EndRefs { SerdeContext& c; flecs::world w; bool own;
                             ~EndRefs() { endEntityRefs(c, w, own); } } endRefs{ctx, e.world(), ownRefs};
            cbor::Reader r(data, size);
            r.seek(obj.body);
            for (uint64_t n = 0; n < obj.item.count; ++n) {
                cbor::Item key;
                if (!r.head(key) || key.kind != cbor::Kind::Text) return false;
                const std::size_t vpos = r.pos();
                if (key.text == kRevisionKey) return false;
                std::size_t k = 0;
                while (k < nf && key.text != (*fields)[k].key) ++k;
                if (k < nf) {
                    if (k < 64) {
                        if (seenBits & (1ull << k)) return false;
                        seenBits |= 1ull << k;
                    }
                    const CborValue v(data, size, vpos);
                    (*fields)[k].loadCbor(t, v, e, ctx);
                    if (v.endPos != SIZE_MAX) { r.seek(v.endPos); continue; }
                }
                r.seek(vpos);
                if (!r.skip()) return false;
            }
            if (r.failed()) return false;
            e.set<T>(std::move(t));
            if (end) *end = r.pos();
            return true;
        };
        m_s.pack = [fields, base](const nlohmann::json& j, packed::Writer& w) {
            if (!j.is_object()) return false;
            // Read as the JSON reader reads it, into a component of its own;
            // entity fields defer into a list nothing applies.
            SerdeContext& ctx = packContext();
            T t = *base;
            for (const Field& f : *fields)
                if (auto it = j.find(f.key); it != j.end())
                    f.loadJson(t, JsonValue{&*it}, flecs::entity(), ctx);
            for (const Field& f : *fields) {
                auto it = j.find(f.key);
                f.pack(t, it != j.end() ? &*it : nullptr, w);
            }
            return true;
        };
        m_s.unpack = [fields, base](flecs::entity e, packed::Reader& r, SerdeContext& ctx) {
            T t = *base;
            const bool ownRefs = beginEntityRefs(ctx);
            struct EndRefs { SerdeContext& c; flecs::world w; bool own;
                             ~EndRefs() { endEntityRefs(c, w, own); } } endRefs{ctx, e.world(), ownRefs};
            for (const Field& f : *fields) f.unpack(t, r, e, ctx);
            if (!r.ok() || !r.atEnd()) return false;
            e.set<T>(std::move(t));
            return true;
        };
        m_s.packedFields = m_s.fields;
        m_s.orderFree = true;
        return std::move(m_s);
    }

private:
    using SaveFn = std::function<void(const T&, nlohmann::json&, flecs::entity owner)>;
    using JsonFn = std::function<void(T&, const JsonValue&, flecs::entity owner, SerdeContext&)>;
    using CborFn = std::function<void(T&, const CborValue&, flecs::entity owner, SerdeContext&)>;
    // Packed: write the field from the component as loaded (the JSON value, for
    // an entity field, whose packed form is the id it names); read it back.
    using PackFn   = std::function<void(const T&, const nlohmann::json* value, packed::Writer&)>;
    using UnpackFn = std::function<void(T&, packed::Reader&, flecs::entity owner, SerdeContext&)>;
    struct Field { std::string key; SaveFn save; JsonFn loadJson; CborFn loadCbor;
                   PackFn pack; UnpackFn unpack; };

    template <class Read>
    SchemaBuilder& add(const char* key, FieldType type, uint32_t count, SaveFn save, Read read,
                       PackFn pack, UnpackFn unpack) {
        m_s.fields.push_back({key, type, count});
        m_fields.push_back({key, std::move(save),
            [read](T& t, const JsonValue& v, flecs::entity o, SerdeContext& c) { read(t, v, o, c); },
            [read](T& t, const CborValue& v, flecs::entity o, SerdeContext& c) { read(t, v, o, c); },
            std::move(pack), std::move(unpack)});
        return *this;
    }

    template <std::size_t N, class M>
    SchemaBuilder& floatArray(const char* key, M T::*m, FieldType type) {
        static_assert(sizeof(M) == N * sizeof(float) && std::is_standard_layout_v<M>,
                      "this field needs a member of exactly N floats");
        return add(key, type, type == FieldType::F32 ? (uint32_t)N : 1,
            [key, m](const T& t, nlohmann::json& j, flecs::entity) {
                const float* f = reinterpret_cast<const float*>(&(t.*m));
                nlohmann::json a = nlohmann::json::array();
                for (std::size_t i = 0; i < N; ++i) a.push_back(f[i]);
                j[key] = std::move(a); },
            ENGINE_FIELD_READ {
                // As jsonread::readFloats: an array; as many as it has, up to N;
                // each a finite number, or that element keeps its default.
                (void)owner; (void)ctx;
                if (!v.isArray()) return;
                float* out = reinterpret_cast<float*>(&(t.*m));
                const std::size_t n = v.size() < N ? v.size() : N;
                v.elements(n, [out](std::size_t i, const auto& e) {
                    if (!e.isNumber()) return;
                    const float f = e.template number<float>();
                    if (std::isfinite(f)) out[i] = f;
                }); },
            [m](const T& t, const nlohmann::json*, packed::Writer& w) { w.raw(&(t.*m), N * 4); },
            [m](T& t, packed::Reader& r, flecs::entity, SerdeContext&) { r.raw(&(t.*m), N * 4); });
    }
#undef ENGINE_FIELD_READ

    template <class I>
    static constexpr FieldType integerType() {
        if constexpr (std::is_same_v<I, int8_t>)   return FieldType::I8;
        else if constexpr (std::is_same_v<I, uint8_t>)  return FieldType::U8;
        else if constexpr (std::is_same_v<I, int16_t>)  return FieldType::I16;
        else if constexpr (std::is_same_v<I, uint16_t>) return FieldType::U16;
        else if constexpr (std::is_same_v<I, int32_t>)  return FieldType::I32;
        else if constexpr (std::is_same_v<I, uint32_t>) return FieldType::U32;
        else if constexpr (std::is_signed_v<I>)         return FieldType::I64;
        else                                            return FieldType::U64;
    }

    static constexpr uint32_t revisionOf() {
        if constexpr (requires { T::kAbiRevision; }) return T::kAbiRevision;
        else return 0;
    }

    ComponentSchema          m_s;
    std::vector<Field>       m_fields;
    std::shared_ptr<const T> m_defaults;
};

}  // namespace EntitySerde
