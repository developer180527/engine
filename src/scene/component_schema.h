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
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "core/json_read.h"
#include "core/logger.h"

namespace EntitySerde {

struct SerdeContext;   // entity_serializer.h: mode, asset hooks, id index

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

// ── Field-by-field schemas ──────────────────────────────────────────────────
// Reads are tolerant per field, the rule every scene reader here follows: a
// field that is missing, of the wrong type or not finite keeps the component's
// default, and the rest of the component still loads. Data that is not an
// object at all is refused, so the component is skipped as before.
template <class T>
class SchemaBuilder {
public:
    SchemaBuilder(std::string name, std::string key) {
        m_s.name = std::move(name);
        m_s.key  = std::move(key);
        m_s.revision = revisionOf();
        m_s.has    = [](flecs::entity e) { return e.try_get<T>() != nullptr; };
        m_s.remove = [](flecs::entity e) { e.remove<T>(); };
    }

    SchemaBuilder& f32(const char* key, float T::*m) {
        m_fields.push_back({
            [key, m](const T& t, nlohmann::json& j) { j[key] = t.*m; },
            [key, m](T& t, const nlohmann::json& j) { t.*m = jsonread::readFloat(j, key, t.*m); }});
        return *this;
    }
    // N floats saved as an array: a float[N], or a vector or quaternion type that
    // is exactly N floats.
    template <std::size_t N, class M>
    SchemaBuilder& floats(const char* key, M T::*m) {
        static_assert(sizeof(M) == N * sizeof(float) && std::is_standard_layout_v<M>,
                      "floats<N> needs a member of exactly N floats");
        m_fields.push_back({
            [key, m](const T& t, nlohmann::json& j) {
                const float* f = reinterpret_cast<const float*>(&(t.*m));
                nlohmann::json a = nlohmann::json::array();
                for (std::size_t i = 0; i < N; ++i) a.push_back(f[i]);
                j[key] = std::move(a);
            },
            [key, m](T& t, const nlohmann::json& j) {
                jsonread::readFloats(j, key, reinterpret_cast<float*>(&(t.*m)), (int)N);
            }});
        return *this;
    }
    SchemaBuilder& boolean(const char* key, bool T::*m) {
        m_fields.push_back({
            [key, m](const T& t, nlohmann::json& j) { j[key] = t.*m; },
            [key, m](T& t, const nlohmann::json& j) { t.*m = jsonread::readBool(j, key, t.*m); }});
        return *this;
    }
    // An enum saved as its integer value. A value outside the enum is kept as
    // read: whoever uses it refuses it, so a scene from a newer engine round-trips.
    template <class E>
    SchemaBuilder& enumeration(const char* key, E T::*m) {
        static_assert(std::is_enum_v<E>);
        m_fields.push_back({
            [key, m](const T& t, nlohmann::json& j) { j[key] = (int)(t.*m); },
            [key, m](T& t, const nlohmann::json& j) {
                if (j.contains(key) && j[key].is_number()) t.*m = (E)j[key].template get<int>();
            }});
        return *this;
    }
    // A byte: read only when it is an unsigned value that fits.
    SchemaBuilder& u8(const char* key, uint8_t T::*m) {
        m_fields.push_back({
            [key, m](const T& t, nlohmann::json& j) { j[key] = t.*m; },
            [key, m](T& t, const nlohmann::json& j) {
                if (!j.contains(key) || !j[key].is_number_unsigned()) return;
                const uint64_t v = j[key].template get<uint64_t>();
                if (v <= 255) t.*m = (uint8_t)v;
            }});
        return *this;
    }
    // A string; `omitEmpty` leaves an empty one out of the saved object.
    SchemaBuilder& string(const char* key, std::string T::*m, bool omitEmpty = false) {
        m_fields.push_back({
            [key, m, omitEmpty](const T& t, nlohmann::json& j) {
                if (!omitEmpty || !(t.*m).empty()) j[key] = t.*m;
            },
            [key, m](T& t, const nlohmann::json& j) { t.*m = jsonread::readString(j, key, t.*m); }});
        return *this;
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

    ComponentSchema build() {
        auto fields = std::make_shared<const std::vector<Field>>(std::move(m_fields));
        m_s.save = [fields](flecs::entity e, nlohmann::json& j, const SerdeContext&) {
            const T* t = e.try_get<T>(); if (!t) return;
            j = nlohmann::json::object();
            for (const Field& f : *fields) f.save(*t, j);
        };
        m_s.load = [fields](flecs::entity e, const nlohmann::json& j, SerdeContext&) {
            if (!j.is_object()) throw std::runtime_error("not an object");
            T t{};
            for (const Field& f : *fields) f.load(t, j);
            e.set<T>(std::move(t));
        };
        return std::move(m_s);
    }

private:
    struct Field {
        std::function<void(const T&, nlohmann::json&)> save;
        std::function<void(T&, const nlohmann::json&)>  load;
    };

    static constexpr uint32_t revisionOf() {
        if constexpr (requires { T::kAbiRevision; }) return T::kAbiRevision;
        else return 0;
    }

    ComponentSchema    m_s;
    std::vector<Field> m_fields;
};

}  // namespace EntitySerde
