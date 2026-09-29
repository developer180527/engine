#pragma once
// ── Inspector model — editing components, without a GUI ─────────────────────
//
// Two things every inspector front end needs and neither should reinvent:
//
//  1. EDIT TRANSACTIONS. A drag across a number field changes the value on
//     every frame, but it is ONE edit and must be ONE undo step. The ImGui
//     inspector got that from ImGui's "item activated" / "deactivated after
//     edit" signals, inside the drawing code — so the undo rule was ImGui's.
//     Here it is begin() / commit() / cancel(), and each front end maps its
//     own widget signals onto those (ImGui: activated/deactivated; libgui: the
//     response's active flag rising and falling).
//
//  2. REFLECTED FIELDS. A kit component has no hand-written section; its
//     meta schema says what it holds. forEachField walks that schema into
//     typed fields, so every front end draws kit components with no code per
//     component — the same promise the ImGui inspector already made.
#include "editor/engine_context.h"
#include "editor/undo_stack.h"
#include "scene/reflected_serde.h"

#include <bx/math.h>
#include <flecs.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <vector>

namespace inspect {

// ── Rotation at the UI boundary ─────────────────────────────────────────────
// Transforms store quaternions; people edit Euler degrees.
inline bx::Vec3 quatToEulerDeg(const bx::Quaternion& q) {
    const float sinp = 2.0f * (q.w * q.x - q.y * q.z);
    float pitch;
    if      (sinp >=  1.0f) pitch =  bx::kPiHalf;
    else if (sinp <= -1.0f) pitch = -bx::kPiHalf;
    else                    pitch = std::asin(sinp);
    const float yaw  = std::atan2(2.0f * (q.w * q.y + q.x * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    const float roll = std::atan2(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.z * q.z + q.x * q.x));
    constexpr float kRadToDeg = 57.2957795f;
    return { pitch * kRadToDeg, yaw * kRadToDeg, roll * kRadToDeg };
}

inline bx::Quaternion eulerDegToQuat(const bx::Vec3& eulerDeg) {
    constexpr float kDegToRad = 0.01745329f;
    const bx::Quaternion qPitch = bx::fromAxisAngle({1, 0, 0}, eulerDeg.x * kDegToRad);
    const bx::Quaternion qYaw   = bx::fromAxisAngle({0, 1, 0}, eulerDeg.y * kDegToRad);
    const bx::Quaternion qRoll  = bx::fromAxisAngle({0, 0, 1}, eulerDeg.z * kDegToRad);
    return bx::normalize(bx::mul(qYaw, bx::mul(qPitch, qRoll)));
}

// ── One continuous edit of a serialised component = one undo step ───────────
// `compKey` is the undo stack's component key ("name", "light", ...).
class PropertyEdit {
public:
    void begin(flecs::entity e, const char* compKey) {
        m_entity = e;
        m_key    = compKey;
        m_before = UndoStack::snapshotComponent(e, compKey);
        m_active = true;
    }
    // Pushes an undo step if the component actually changed, marks the scene
    // dirty either way (a no-op edit is still an edit attempt). False if no
    // edit was in progress.
    bool commit(EngineContext& ctx, const char* description) {
        if (!m_active) return false;
        m_active = false;
        if (!m_entity.is_alive()) return false;
        auto after = UndoStack::snapshotComponent(m_entity, m_key.c_str());
        if (after != m_before)
            ctx.editor.undoStack.pushPropertyEdit(m_entity, m_key.c_str(), m_before, after, description);
        ctx.editor.sceneDirty = true;
        return true;
    }
    void cancel() { m_active = false; }
    bool active() const { return m_active; }

    // The shape every front end has: "did the widget start being edited this
    // frame", "did it stop, having changed something", "did it stop at all".
    void track(EngineContext& ctx, flecs::entity e, const char* compKey, const char* description,
               bool started, bool endedAfterEdit, bool ended) {
        if (started) begin(e, compKey);
        if (m_active && endedAfterEdit)  commit(ctx, description);
        else if (m_active && ended)      cancel();
    }

private:
    flecs::entity  m_entity;
    std::string    m_key;
    nlohmann::json m_before;
    bool           m_active = false;
};

// Transform takes the undo stack's lightweight path rather than a JSON
// snapshot: it is edited far more than anything else (gizmo, fields).
class TransformEdit {
public:
    void begin(flecs::entity e) {
        m_entity = e;
        if (const Transform* t = e.try_get<Transform>()) m_before = *t;
        m_active = true;
    }
    bool commit(EngineContext& ctx) {
        if (!m_active) return false;
        m_active = false;
        if (!m_entity.is_alive()) return false;
        if (const Transform* t = m_entity.try_get<Transform>())
            ctx.editor.undoStack.pushTransform(m_entity, m_before, *t);
        ctx.editor.sceneDirty = true;
        return true;
    }
    void cancel() { m_active = false; }
    bool active() const { return m_active; }

    void track(EngineContext& ctx, flecs::entity e, bool started, bool endedAfterEdit, bool ended) {
        if (started) begin(e);
        if (m_active && endedAfterEdit)  commit(ctx);
        else if (m_active && ended)      cancel();
    }

private:
    flecs::entity m_entity;
    Transform     m_before{};
    bool          m_active = false;
};

// ── Reflected (meta-schema) components ──────────────────────────────────────
enum class FieldKind { F32, F64, Bool, I32, U32, I64, U64, Struct, Unsupported };

struct Field {
    const char*   name  = "";
    FieldKind     kind  = FieldKind::Unsupported;
    void*         ptr   = nullptr;   // into the component's storage
    flecs::entity type;              // the member's type (for Struct)
    int           depth = 0;
};

// Each member of `type` at `base`, depth-first; nested structs (a vec3
// member) are entered one level, as the ImGui inspector always did. `f` is
// called for the struct itself (kind Struct) and then for its members.
template <class F>
void forEachField(flecs::world& w, flecs::entity type, void* base, F&& f, int depth = 0) {
    const EcsStruct* st = static_cast<const EcsStruct*>(ecs_get_id(w, type, ecs_id(EcsStruct)));
    if (!st) return;
    const ecs_member_t* members = ecs_vec_first_t(&st->members, ecs_member_t);
    const int32_t       count   = ecs_vec_count(&st->members);
    for (int32_t i = 0; i < count; ++i) {
        const ecs_member_t& m = members[i];
        Field fd;
        fd.name  = m.name;
        fd.ptr   = static_cast<char*>(base) + m.offset;
        fd.type  = flecs::entity(w, m.type);
        fd.depth = depth;
        if (const EcsPrimitive* prim = static_cast<const EcsPrimitive*>(
                ecs_get_id(w, m.type, ecs_id(EcsPrimitive)))) {
            switch (prim->kind) {
            case EcsF32:  fd.kind = FieldKind::F32;  break;
            case EcsF64:  fd.kind = FieldKind::F64;  break;
            case EcsBool: fd.kind = FieldKind::Bool; break;
            case EcsI32:  fd.kind = FieldKind::I32;  break;
            case EcsU32:  fd.kind = FieldKind::U32;  break;
            case EcsI64:  fd.kind = FieldKind::I64;  break;
            case EcsU64:  fd.kind = FieldKind::U64;  break;
            default:      fd.kind = FieldKind::Unsupported; break;
            }
            f(fd);
        } else if (depth < 1 && ecs_has_id(w, m.type, ecs_id(EcsStruct))) {
            fd.kind = FieldKind::Struct;
            f(fd);
            forEachField(w, fd.type, fd.ptr, f, depth + 1);
        } else {
            fd.kind = FieldKind::Unsupported;
            f(fd);
        }
    }
}

struct ReflectedComponent {
    flecs::entity type;
    std::string   path;    // what the section is titled
};

// The meta-registered components on `e` with no hand-written section,
// collected (not iterated live) so a front end may remove one while drawing.
inline std::vector<ReflectedComponent> reflectedComponents(flecs::entity e) {
    std::vector<ReflectedComponent> out;
    e.each([&](flecs::id id) {
        if (reflected::isReflectable(id))
            out.push_back({id.entity(), reflected::componentPath(id.entity())});
    });
    return out;
}

// Storage for a reflected component, created if needed.
inline void* componentPtr(flecs::entity e, flecs::entity type) {
    flecs::world w = e.world();
    return reflected::ensurePtr(w, e, type);
}

// After writing through a Field's pointer: tell flecs, dirty the scene.
// (Reflected edits do not reach the undo stack yet: its snapshot API is keyed
// to the hand-written serde table — a known v1 limit of the ImGui inspector
// too.)
inline void markEdited(EngineContext& ctx, flecs::entity e, flecs::entity type) {
    ecs_modified_id(e.world(), e, type);
    ctx.editor.sceneDirty = true;
}

inline void removeComponent(EngineContext& ctx, flecs::entity e, flecs::entity type) {
    ecs_remove_id(e.world(), e, type);
    ctx.editor.sceneDirty = true;
}

// Components whose kit is not loaded yet: their paths, for a read-only list.
inline std::vector<std::string> pendingComponents(flecs::entity e) {
    std::vector<std::string> out;
    if (const auto* p = e.try_get<reflected::ReflectedPending>())
        for (const auto& [path, blob] : p->blobs) out.push_back(path);
    return out;
}

}  // namespace inspect
