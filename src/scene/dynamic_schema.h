#pragma once
// ── dynamic_schema — a scene schema for a component known only at run time ───
// WO-073. SchemaBuilder<T> (component_schema.h) describes a C++ type through
// member pointers. A component registered through the C API
// (engRegisterComponent) has no C++ type: only a size and a list of fields at
// byte offsets. This builds the same ComponentSchema from that list, so such a
// component is saved, loaded and cooked with scenes like any engine one.
//
// The reading rules are SchemaBuilder's: a field missing, of the wrong type,
// out of range or not finite keeps the component's default, and the rest
// loads. An entity field saves the target's stable id and is linked once the
// whole scene exists. Not packed (no `pack`): the cooker keeps such a
// component as CBOR, which the packed loader reads.
//
// Field types here are the C API's (EngFieldType); asset and blob fields are
// refused before this is reached.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "scene/component_schema.h"

namespace EntitySerde {

struct DynamicField {
    std::string name;
    uint32_t    type   = 0;   // EngFieldType
    uint32_t    offset = 0;
    uint32_t    count  = 1;   // values; for FixedString, bytes
};

namespace dyn {
// EngFieldType values (eng_world.h), repeated so this header needs no C API.
enum : uint32_t {
    Bool32 = 0, I32, U32, I64, U64, F32, F64, Vec3, Quat, Entity, Asset, FixedString,
    U8, I8, U16, I16, Vec2, Vec4, Mat4, Blob
};
// Bytes one value takes; 0 for a type this cannot hold.
inline uint32_t valueSize(uint32_t t) {
    switch (t) {
        case Bool32: case I32: case U32: case F32: return 4;
        case I64: case U64: case F64: case Entity: return 8;
        case U8: case I8: case FixedString: return 1;
        case U16: case I16: return 2;
        case Vec2: return 8;
        case Vec3: return 12;
        case Vec4: case Quat: return 16;
        case Mat4: return 64;
        default: return 0;
    }
}
inline uint32_t floatsOf(uint32_t t) {
    switch (t) { case Vec2: return 2; case Vec3: return 3; case Vec4: case Quat: return 4;
                 case Mat4: return 16; case F32: return 1; default: return 0; }
}
inline FieldType schemaType(uint32_t t) {
    switch (t) {
        case Bool32: return FieldType::Bool;  case I8: return FieldType::I8;
        case U8: return FieldType::U8;        case I16: return FieldType::I16;
        case U16: return FieldType::U16;      case I32: return FieldType::I32;
        case U32: return FieldType::U32;      case I64: return FieldType::I64;
        case U64: return FieldType::U64;      case F32: return FieldType::F32;
        case F64: return FieldType::F64;      case Vec2: return FieldType::Vec2;
        case Vec3: return FieldType::Vec3;    case Vec4: return FieldType::Vec4;
        case Quat: return FieldType::Quat;    case Mat4: return FieldType::Mat4;
        case FixedString: return FieldType::String;
        default: return FieldType::Entity;
    }
}

template <class I> inline void writeInt(uint8_t* p, const nlohmann::json& v) {
    if (v.is_number_unsigned()) {
        const uint64_t u = v.get<uint64_t>();
        if (u <= (uint64_t)std::numeric_limits<I>::max()) { const I x = (I)u; std::memcpy(p, &x, sizeof x); }
    } else if (v.is_number_integer()) {
        const int64_t s = v.get<int64_t>();
        if constexpr (std::is_signed_v<I>) {
            if (s >= (int64_t)std::numeric_limits<I>::min() && s <= (int64_t)std::numeric_limits<I>::max()) {
                const I x = (I)s; std::memcpy(p, &x, sizeof x); }
        } else if (s >= 0 && (uint64_t)s <= (uint64_t)std::numeric_limits<I>::max()) {
            const I x = (I)s; std::memcpy(p, &x, sizeof x); }
    }
}
template <class I> inline nlohmann::json readInt(const uint8_t* p) { I x; std::memcpy(&x, p, sizeof x); return x; }

// One value of type t at p, as JSON.
inline nlohmann::json saveValue(uint32_t t, const uint8_t* p) {
    switch (t) {
        case Bool32: { uint32_t b; std::memcpy(&b, p, 4); return b != 0; }
        case I8:  return readInt<int8_t>(p);   case U8:  return readInt<uint8_t>(p);
        case I16: return readInt<int16_t>(p);  case U16: return readInt<uint16_t>(p);
        case I32: return readInt<int32_t>(p);  case U32: return readInt<uint32_t>(p);
        case I64: return readInt<int64_t>(p);  case U64: return readInt<uint64_t>(p);
        case F64: { double d; std::memcpy(&d, p, 8); return d; }
        case F32: { float f; std::memcpy(&f, p, 4); return f; }
        default: {
            nlohmann::json a = nlohmann::json::array();
            for (uint32_t i = 0; i < floatsOf(t); ++i) { float f; std::memcpy(&f, p + 4 * i, 4); a.push_back(f); }
            return a;
        }
    }
}
// One value of type t from JSON into p; anything unreadable leaves p as it was.
inline void loadValue(uint32_t t, uint8_t* p, const nlohmann::json& v) {
    switch (t) {
        case Bool32: if (v.is_boolean()) { const uint32_t b = v.get<bool>() ? 1u : 0u; std::memcpy(p, &b, 4); } return;
        case I8:  writeInt<int8_t>(p, v); return;   case U8:  writeInt<uint8_t>(p, v); return;
        case I16: writeInt<int16_t>(p, v); return;  case U16: writeInt<uint16_t>(p, v); return;
        case I32: writeInt<int32_t>(p, v); return;  case U32: writeInt<uint32_t>(p, v); return;
        case I64: writeInt<int64_t>(p, v); return;  case U64: writeInt<uint64_t>(p, v); return;
        case F64: if (v.is_number()) { const double d = v.get<double>(); if (std::isfinite(d)) std::memcpy(p, &d, 8); } return;
        case F32: if (v.is_number()) { const float f = v.get<float>(); if (std::isfinite(f)) std::memcpy(p, &f, 4); } return;
        default: {
            const uint32_t n = floatsOf(t);
            if (!v.is_array() || v.size() != n) return;
            float tmp[16];
            for (uint32_t i = 0; i < n; ++i) {
                if (!v[i].is_number()) return;
                tmp[i] = v[i].get<float>();
                if (!std::isfinite(tmp[i])) return;
            }
            std::memcpy(p, tmp, 4 * n);
        }
    }
}
} // namespace dyn

// The schema for component `id` of `size` bytes, saved under `name` (also its
// key on disk). `defaults` (size bytes, or empty: zero) is what a load starts
// from when the entity does not have the component yet.
inline ComponentSchema dynamicSchema(const std::string& name, uint32_t revision, flecs::entity_t id,
                                     uint32_t size, std::vector<DynamicField> fields,
                                     std::vector<uint8_t> defaults) {
    ComponentSchema s;
    s.name = name;
    s.key = name;
    s.revision = revision;
    if (defaults.size() != size) defaults.assign(size, 0);
    for (const DynamicField& f : fields) {
        FieldInfo fi;
        fi.name = f.name;
        fi.type = dyn::schemaType(f.type);
        fi.count = f.type == dyn::FixedString ? 1 : f.count;
        s.fields.push_back(fi);
    }
    s.has    = [id](flecs::entity e) { return ecs_has_id(e.world().c_ptr(), e.id(), id); };
    s.remove = [id](flecs::entity e) { ecs_remove_id(e.world().c_ptr(), e.id(), id); };

    s.save = [id, size, fields](flecs::entity e, nlohmann::json& out, const SerdeContext&) {
        const uint8_t* b = static_cast<const uint8_t*>(ecs_get_id(e.world().c_ptr(), e.id(), id));
        out = nlohmann::json::object();
        if (!b) return;
        for (const DynamicField& f : fields) {
            const uint8_t* p = b + f.offset;
            if (f.type == dyn::FixedString) {
                out[f.name] = std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), f.count));
                continue;
            }
            const uint32_t vs = dyn::valueSize(f.type);
            auto one = [&](const uint8_t* q) -> nlohmann::json {
                if (f.type != dyn::Entity) return dyn::saveValue(f.type, q);
                uint64_t target; std::memcpy(&target, q, 8);
                return target && ecs_is_alive(e.world().c_ptr(), target)
                     ? stableIdOf(flecs::entity(e.world(), target)) : (uint64_t)0;
            };
            if (f.count <= 1) { out[f.name] = one(p); continue; }
            nlohmann::json a = nlohmann::json::array();
            for (uint32_t i = 0; i < f.count; ++i) a.push_back(one(p + i * vs));
            out[f.name] = std::move(a);
        }
    };

    s.load = [id, size, fields, defaults](flecs::entity e, const nlohmann::json& in, SerdeContext& ctx) {
        if (!in.is_object()) throw std::runtime_error("not an object");
        flecs::world_t* w = e.world().c_ptr();
        std::vector<uint8_t> b = defaults;
        if (const void* have = ecs_get_id(w, e.id(), id)) std::memcpy(b.data(), have, size);
        for (const DynamicField& f : fields) {
            auto it = in.find(f.name);
            if (it == in.end()) continue;
            uint8_t* p = b.data() + f.offset;
            if (f.type == dyn::FixedString) {
                if (!it->is_string()) continue;
                const std::string str = it->get<std::string>();
                std::memset(p, 0, f.count);
                std::memcpy(p, str.data(), std::min<size_t>(str.size(), f.count - 1));
                continue;
            }
            const uint32_t vs = dyn::valueSize(f.type);
            auto one = [&](uint32_t index, const nlohmann::json& v) {
                uint8_t* q = p + index * vs;
                if (f.type != dyn::Entity) { dyn::loadValue(f.type, q, v); return; }
                std::memset(q, 0, 8);   // never a flecs id from another world or session
                const uint64_t sid = u64From(JsonValue{&v});   // signed or unsigned, as SchemaBuilder reads it
                if (!sid) return;
                const uint32_t at = f.offset + index * 8;
                deferEntityRef(ctx, e, sid, [id, at](flecs::entity o, flecs::entity target) {
                    flecs::world_t* ow = o.world().c_ptr();
                    if (void* c = ecs_get_mut_id(ow, o.id(), id)) {
                        const uint64_t t = target.id();
                        std::memcpy(static_cast<uint8_t*>(c) + at, &t, 8);
                        ecs_modified_id(ow, o.id(), id);
                    }
                });
            };
            if (f.count <= 1) { one(0, *it); continue; }
            if (!it->is_array()) continue;
            for (uint32_t i = 0; i < f.count && i < it->size(); ++i) one(i, (*it)[i]);
        }
        ecs_set_id(w, e.id(), id, size, b.data());
    };
    return s;
}

} // namespace EntitySerde
