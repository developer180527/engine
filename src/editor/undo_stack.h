#pragma once
#include <string>
#include <deque>
#include <nlohmann/json.hpp>
#include <flecs.h>
#include "core/transform.h"
#include "core/transform_utils.h"
#include "components/name.h"
#include "components/entity_id.h"
#include "core/entity_id_util.h"
#include "scene/entity_serializer.h"
#include "core/logger.h"

// ── UndoStack ──────────────────────────────────────────────────────────────
// Session-only. Entity references key on the stable EntityId. Full-entity
// snapshots (add/delete) run through the SAME EntitySerde table as scene save,
// so undo can no longer drift from the serializer: restoring a deleted entity
// brings back every serialized component, not the partial set the old
// hand-written snapshot captured. Respawn preserves the original id, so other
// commands still in the stack continue to resolve the entity.
enum class UndoCmdType { Transform, EntityAdd, EntityDelete, Reparent, PropertyEdit, ComponentToggle };

struct UndoCommand {
    UndoCmdType    type;
    std::string    description;
    nlohmann::json before;
    nlohmann::json after;
};

class UndoStack {
public:
    static constexpr int kMaxDepth = 200;

    void push(UndoCommand cmd) {
        if (m_index < (int)m_history.size() - 1)
            m_history.erase(m_history.begin() + m_index + 1, m_history.end());
        m_history.push_back(std::move(cmd));
        m_index = (int)m_history.size() - 1;
        while ((int)m_history.size() > kMaxDepth) {
            m_history.pop_front();
            if (m_index > 0) --m_index;
        }
    }

    bool undo(flecs::world& ecs) { if (!canUndo()) return false; apply(ecs, m_history[m_index], true);  --m_index; return true; }
    bool redo(flecs::world& ecs) { if (!canRedo()) return false; ++m_index; apply(ecs, m_history[m_index], false); return true; }

    bool        canUndo()         const { return m_index >= 0; }
    bool        canRedo()         const { return m_index < (int)m_history.size()-1; }
    std::string undoDescription() const { return canUndo() ? m_history[m_index].description : ""; }
    std::string redoDescription() const { return canRedo() ? m_history[m_index+1].description : ""; }
    void        clear()                 { m_history.clear(); m_index = -1; }

    // ── Factory helpers (entities referenced by stable id) ──────────────────
    void pushTransform(flecs::entity e, const Transform& before, const Transform& after) {
        if (!e.is_alive()) return;
        uint64_t id = ensureEntityId(e);
        UndoCommand c; c.type = UndoCmdType::Transform;
        const Name* n = e.try_get<Name>();
        c.description = std::string("Move ") + (n ? n->value : "entity");
        c.before = serTf(before); c.before["id"] = id;
        c.after  = serTf(after);  c.after["id"]  = id;
        push(std::move(c));
    }
    void pushEntityAdd(flecs::entity e) {
        if (!e.is_alive()) return;
        ensureEntityId(e);   // lazy ids: snapshot must capture a real id, else undo can't find it
        if (flecs::entity p = e.target(flecs::ChildOf); p && p.is_alive()) ensureEntityId(p);
        UndoCommand c; c.type = UndoCmdType::EntityAdd;
        c.after = snapshot(e);
        c.description = "Add " + c.after.value("name", std::string("entity"));
        push(std::move(c));
    }
    void pushEntityDelete(flecs::entity e) {   // call BEFORE destruct
        if (!e.is_alive()) return;
        ensureEntityId(e);   // lazy ids: snapshot must capture a real id, else undo can't find it
        if (flecs::entity p = e.target(flecs::ChildOf); p && p.is_alive()) ensureEntityId(p);
        UndoCommand c; c.type = UndoCmdType::EntityDelete;
        c.before = snapshot(e);
        c.description = "Delete " + c.before.value("name", std::string("entity"));
        push(std::move(c));
    }
    void pushReparent(flecs::entity child, uint64_t oldParentId, const Transform& oldLocal,
                      uint64_t newParentId, const Transform& newLocal) {
        if (!child.is_alive()) return;
        uint64_t cid = ensureEntityId(child);
        UndoCommand c; c.type = UndoCmdType::Reparent;
        const Name* n = child.try_get<Name>();
        c.description = std::string("Reparent ") + (n ? n->value : "entity");
        c.before = { {"id", cid}, {"parentId", oldParentId}, {"transform", serTf(oldLocal)} };
        c.after  = { {"id", cid}, {"parentId", newParentId}, {"transform", serTf(newLocal)} };
        push(std::move(c));
    }

    // ── Component snapshot (public — used by inspector capture) ─────────────
    static nlohmann::json snapshotComponent(flecs::entity e, const char* key) {
        EntitySerde::SerdeContext sctx; sctx.mode = EntitySerde::SerdeMode::Memory;
        for (const auto& d : EntitySerde::table()) {
            if (std::string(d.key) == key && d.has(e)) {
                nlohmann::json j; d.save(e, j, sctx); return j;
            }
        }
        return {};
    }

    void pushPropertyEdit(flecs::entity e, const char* compKey,
                          const nlohmann::json& before, const nlohmann::json& after,
                          const std::string& desc) {
        if (!e.is_alive() || before == after) return;
        uint64_t id = ensureEntityId(e);
        UndoCommand c; c.type = UndoCmdType::PropertyEdit; c.description = desc;
        c.before = { {"id", id}, {"key", compKey}, {"data", before} };
        c.after  = { {"id", id}, {"key", compKey}, {"data", after}  };
        push(std::move(c));
    }

    void pushComponentAdd(flecs::entity e, const char* compKey, const std::string& desc) {
        if (!e.is_alive()) return;
        uint64_t id = ensureEntityId(e);
        auto data = snapshotComponent(e, compKey);
        UndoCommand c; c.type = UndoCmdType::ComponentToggle; c.description = desc;
        c.before = { {"id", id}, {"key", compKey}, {"exists", false} };
        c.after  = { {"id", id}, {"key", compKey}, {"exists", true}, {"data", data} };
        push(std::move(c));
    }

    void pushComponentRemove(flecs::entity e, const char* compKey, const std::string& desc) {
        if (!e.is_alive()) return;
        uint64_t id = ensureEntityId(e);
        auto data = snapshotComponent(e, compKey);
        UndoCommand c; c.type = UndoCmdType::ComponentToggle; c.description = desc;
        c.before = { {"id", id}, {"key", compKey}, {"exists", true}, {"data", data} };
        c.after  = { {"id", id}, {"key", compKey}, {"exists", false} };
        push(std::move(c));
    }

private:
    std::deque<UndoCommand> m_history;
    int m_index = -1;

    void apply(flecs::world& ecs, const UndoCommand& cmd, bool reverse) {
        switch (cmd.type) {
        case UndoCmdType::Transform:
            applyTransform(ecs, reverse ? cmd.before : cmd.after); break;
        case UndoCmdType::EntityAdd:
            if (reverse) destroyById(ecs, cmd.after.value("id",(uint64_t)0));
            else         spawnFromSnapshot(ecs, cmd.after); break;
        case UndoCmdType::EntityDelete:
            if (reverse) spawnFromSnapshot(ecs, cmd.before);
            else         destroyById(ecs, cmd.before.value("id",(uint64_t)0)); break;
        case UndoCmdType::Reparent:
            applyReparent(ecs, reverse ? cmd.before : cmd.after); break;
        case UndoCmdType::PropertyEdit:
            applyPropertyEdit(ecs, reverse ? cmd.before : cmd.after); break;
        case UndoCmdType::ComponentToggle:
            applyComponentToggle(ecs, reverse ? cmd.before : cmd.after); break;
        }
    }

    static void applyTransform(flecs::world& ecs, const nlohmann::json& j) {
        flecs::entity e = findById(ecs, j.value("id",(uint64_t)0));
        if (!e.is_alive()) { LOG_WARN("Undo", "transform target id not found"); return; }
        Transform& t = e.get_mut<Transform>(); desTf(j, t);
    }
    static void applyReparent(flecs::world& ecs, const nlohmann::json& j) {
        flecs::entity e = findById(ecs, j.value("id",(uint64_t)0));
        if (!e.is_alive()) return;
        e.remove(flecs::ChildOf, flecs::Wildcard);
        uint64_t pid = j.value("parentId",(uint64_t)0);
        if (pid) {
            flecs::entity p = findById(ecs, pid);
            if (p.is_alive() && p != e && !isAncestorOf(e, p)) e.add(flecs::ChildOf, p);
        }
        if (j.contains("transform")) { Transform& t = e.get_mut<Transform>(); desTf(j["transform"], t); }
    }
    static void destroyById(flecs::world& ecs, uint64_t id) {
        flecs::entity e = findById(ecs, id); if (e.is_alive()) e.destruct();
    }

    static void restoreComponent(flecs::entity e, const std::string& key, const nlohmann::json& data) {
        EntitySerde::SerdeContext sctx; sctx.mode = EntitySerde::SerdeMode::Memory;
        for (const auto& d : EntitySerde::table())
            if (key == d.key) { d.load(e, data, sctx); return; }
    }
    static void removeComponentByKey(flecs::entity e, const std::string& key) {
        if      (key == "camera")               e.remove<Camera>();
        else if (key == "light")                e.remove<Light>();
        else if (key == "rigidBody")            e.remove<RigidBody>();
        else if (key == "characterController")  e.remove<CharacterController>();
        else if (key == "script")               e.remove<ScriptComponent>();
    }
    static void applyPropertyEdit(flecs::world& ecs, const nlohmann::json& j) {
        flecs::entity e = findById(ecs, j.value("id",(uint64_t)0));
        if (!e.is_alive()) { LOG_WARN("Undo","property target not found"); return; }
        restoreComponent(e, j.value("key",std::string{}), j["data"]);
    }
    static void applyComponentToggle(flecs::world& ecs, const nlohmann::json& j) {
        flecs::entity e = findById(ecs, j.value("id",(uint64_t)0));
        if (!e.is_alive()) { LOG_WARN("Undo","component toggle target not found"); return; }
        std::string key = j.value("key",std::string{});
        if (j.value("exists",false)) restoreComponent(e, key, j["data"]);
        else                         removeComponentByKey(e, key);
    }

    // ── Full-entity snapshot / respawn via the shared serializer (Memory) ───
    static nlohmann::json snapshot(flecs::entity e) {
        EntitySerde::SerdeContext ctx; ctx.mode = EntitySerde::SerdeMode::Memory;
        return EntitySerde::saveEntity(e, ctx);
    }
    static void spawnFromSnapshot(flecs::world& ecs, const nlohmann::json& j) {
        uint64_t id = j.value("id", (uint64_t)0);
        if (id && findById(ecs, id).is_alive()) return;          // already present
        EntitySerde::SerdeContext ctx; ctx.mode = EntitySerde::SerdeMode::Memory;
        flecs::entity e = EntitySerde::createEntity(ecs, j, ctx, EntitySerde::IdPolicy::Preserve);
        uint64_t pid = j.value("parentId", (uint64_t)0);         // restore parent link
        if (pid) {
            flecs::entity p = findById(ecs, pid);
            if (p.is_alive() && p != e && !isAncestorOf(e, p)) e.add(flecs::ChildOf, p);
        }
    }

    // ── Bare Transform <-> json (lightweight transform / reparent commands) ─
    static nlohmann::json serTf(const Transform& t) {
        return { {"position", {t.position.x, t.position.y, t.position.z}},
                 {"rotation", {t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w}},
                 {"scale",    {t.scale.x,    t.scale.y,    t.scale.z}} };
    }
    // Through the shared checked reads. Latent rather than live — this JSON is
    // built in-process by serTf, never read from disk — but it is the same
    // undefined behaviour as the other three sites and one hand-authored undo
    // command away from mattering. core/json_read.h explains the defect.
    static void desTf(const nlohmann::json& j, Transform& t) {
        jsonread::readFloats(j, "position", &t.position.x, 3);
        jsonread::readFloats(j, "rotation", &t.rotation.x, 4);
        jsonread::readFloats(j, "scale",    &t.scale.x,    3);
    }
};
