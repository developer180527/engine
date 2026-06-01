#pragma once
#include <lua.hpp>
#include <flecs.h>
#include "engine/scripting/script_host.h"
#include "core/transform.h"
#include "components/name.h"

// ── LuaBindings ────────────────────────────────────────────────────────────
// Binds the ScriptHost contract into a lua_State. Every C function receives
// the ScriptHost* as upvalue 1 (shared via luaL_setfuncs), so bindings are
// installed once at VM creation while the host's world is rebound each Play.
//
// Script-facing surface:
//   entity:getTransform() -> {position={x,y,z}, rotation={x,y,z,w}, scale={x,y,z}}
//   entity:setTransform(t) / :isAlive() / :name() / :destroy()
//   entity:setParent(other) / :clearParent()
//   entity:applyImpulse(x,y,z) / :setVelocity(x,y,z)   (no-op until physics service)
//   Input.keyDown(k) / keyPressed(k) / axis(name) / mouseDown(btn)
//   Log.info/warn/error(msg)      Time.dt()/elapsed()/frame()
//   World.find(name) / World.create(name)
//   Audio.play(path) / playAt(path,x,y,z)   (no-op until audio service)
namespace LuaBindings {

inline ScriptHost* host(lua_State* L) {
    return static_cast<ScriptHost*>(lua_touserdata(L, lua_upvalueindex(1)));
}

static const char* kEntityMT = "Engine.Entity";

// Entity userdata carries a session epoch. The Lua VM survives across
// play sessions, so a ref stashed in a global from a prior Play would
// otherwise alias a different entity now. checkEntity rejects stale refs.
struct EntityRef { uint64_t id; uint32_t epoch; };
inline void pushEntity(lua_State* L, flecs::entity e, ScriptHost* h) {
    EntityRef* p = static_cast<EntityRef*>(lua_newuserdatauv(L, sizeof(EntityRef), 0));
    p->id = (uint64_t)e.id();
    p->epoch = h->epoch();
    luaL_setmetatable(L, kEntityMT);
}
inline flecs::entity checkEntity(lua_State* L, int idx, ScriptHost* h) {
    EntityRef* p = static_cast<EntityRef*>(luaL_checkudata(L, idx, kEntityMT));
    if (p->epoch != h->epoch())
        luaL_error(L, "stale entity reference from a previous play session");
    return h->world()->entity((flecs::entity_t)p->id);
}

// ── Transform marshalling ───────────────────────────────────────────────
inline void pushVec3(lua_State* L, float x, float y, float z) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, z); lua_setfield(L, -2, "z");
}
inline void pushTransform(lua_State* L, const Transform& t) {
    lua_createtable(L, 0, 3);
    pushVec3(L, t.position.x, t.position.y, t.position.z); lua_setfield(L, -2, "position");
    lua_createtable(L, 0, 4);
    lua_pushnumber(L, t.rotation.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, t.rotation.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, t.rotation.z); lua_setfield(L, -2, "z");
    lua_pushnumber(L, t.rotation.w); lua_setfield(L, -2, "w");
    lua_setfield(L, -2, "rotation");
    pushVec3(L, t.scale.x, t.scale.y, t.scale.z); lua_setfield(L, -2, "scale");
}
inline float fieldNum(lua_State* L, int tbl, const char* k, float def) {
    lua_getfield(L, tbl, k);
    float v = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}
inline Transform readTransform(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    Transform t{}; t.position = {0,0,0}; t.rotation = {0,0,0,1}; t.scale = {1,1,1};
    luaL_checktype(L, idx, LUA_TTABLE);
    if (lua_getfield(L, idx, "position") == LUA_TTABLE) {
        int p = lua_absindex(L, -1);
        t.position = { fieldNum(L,p,"x",0), fieldNum(L,p,"y",0), fieldNum(L,p,"z",0) };
    } lua_pop(L, 1);
    if (lua_getfield(L, idx, "rotation") == LUA_TTABLE) {
        int r = lua_absindex(L, -1);
        t.rotation = { fieldNum(L,r,"x",0), fieldNum(L,r,"y",0), fieldNum(L,r,"z",0), fieldNum(L,r,"w",1) };
    } lua_pop(L, 1);
    if (lua_getfield(L, idx, "scale") == LUA_TTABLE) {
        int s = lua_absindex(L, -1);
        t.scale = { fieldNum(L,s,"x",1), fieldNum(L,s,"y",1), fieldNum(L,s,"z",1) };
    } lua_pop(L, 1);
    return t;
}

// ── Entity methods ──────────────────────────────────────────────────────
inline int e_getTransform(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L, 1, h);
    Transform t; if (!h->getTransform(e, t)) { lua_pushnil(L); return 1; }
    pushTransform(L, t); return 1;
}
inline int e_setTransform(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L, 1, h);
    h->setTransform(e, readTransform(L, 2)); return 0;
}
inline int e_isAlive(lua_State* L) {
    ScriptHost* h = host(L); lua_pushboolean(L, h->isAlive(checkEntity(L, 1, h))); return 1;
}
inline int e_name(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L, 1, h);
    const Name* n = e.try_get<Name>(); lua_pushstring(L, n ? n->value.c_str() : ""); return 1;
}
inline int e_destroy(lua_State* L) {
    ScriptHost* h = host(L); h->destroy(checkEntity(L, 1, h)); return 0;
}
inline int e_setParent(lua_State* L) {
    ScriptHost* h = host(L); h->setParent(checkEntity(L,1,h), checkEntity(L,2,h)); return 0;
}
inline int e_clearParent(lua_State* L) {
    ScriptHost* h = host(L); h->clearParent(checkEntity(L,1,h)); return 0;
}
inline int e_applyImpulse(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L,1,h);
    h->applyImpulse(e, (float)luaL_checknumber(L,2), (float)luaL_checknumber(L,3), (float)luaL_checknumber(L,4));
    return 0;
}
inline int e_setVelocity(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L,1,h);
    h->setVelocity(e, (float)luaL_checknumber(L,2), (float)luaL_checknumber(L,3), (float)luaL_checknumber(L,4));
    return 0;
}
// ── Input / Log / Time / World / Audio ──────────────────────────────────
inline int e_move(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L,1,h);
    h->charMove(e, (float)luaL_checknumber(L,2), (float)luaL_checknumber(L,3));
    return 0;
}
inline int e_jump(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L,1,h);
    h->charJump(e, (float)luaL_optnumber(L,2, 5.0));
    return 0;
}
inline int e_isGrounded(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = checkEntity(L,1,h);
    lua_pushboolean(L, h->charGrounded(e)); return 1;
}

inline int in_keyDown(lua_State* L)    { lua_pushboolean(L, host(L)->keyDown(luaL_checkstring(L,1))); return 1; }
inline int in_keyPressed(lua_State* L) { lua_pushboolean(L, host(L)->keyPressed(luaL_checkstring(L,1))); return 1; }
inline int in_axis(lua_State* L)       { lua_pushnumber(L, host(L)->axis(luaL_checkstring(L,1))); return 1; }
inline int in_mouseDown(lua_State* L)  { lua_pushboolean(L, host(L)->mouseDown((int)luaL_checkinteger(L,1))); return 1; }

inline int log_info(lua_State* L)  { host(L)->logInfo (luaL_checkstring(L,1)); return 0; }
inline int log_warn(lua_State* L)  { host(L)->logWarn (luaL_checkstring(L,1)); return 0; }
inline int log_error(lua_State* L) { host(L)->logError(luaL_checkstring(L,1)); return 0; }

inline int time_dt(lua_State* L)      { lua_pushnumber(L, host(L)->dt()); return 1; }
inline int time_elapsed(lua_State* L) { lua_pushnumber(L, host(L)->elapsed()); return 1; }
inline int time_frame(lua_State* L)   { lua_pushinteger(L, (lua_Integer)host(L)->frame()); return 1; }

inline int world_find(lua_State* L) {
    ScriptHost* h = host(L); flecs::entity e = h->find(luaL_checkstring(L,1));
    if (e.is_alive()) pushEntity(L, e, h); else lua_pushnil(L); return 1;
}
inline int world_create(lua_State* L) {
    ScriptHost* h = host(L); pushEntity(L, h->create(luaL_checkstring(L,1)), h); return 1;
}

inline int audio_play(lua_State* L)   { lua_pushinteger(L, (lua_Integer)host(L)->playSound(luaL_checkstring(L,1))); return 1; }
inline int audio_playAt(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)host(L)->playSoundAt(luaL_checkstring(L,1),
        (float)luaL_checknumber(L,2), (float)luaL_checknumber(L,3), (float)luaL_checknumber(L,4))); return 1;
}

// Physics.raycast(origin{x,y,z}, dir{x,y,z}, maxDist?) ->
//   { hit, distance, point{x,y,z}, normal{x,y,z}, entity }
inline int phys_raycast(lua_State* L) {
    ScriptHost* h = host(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    int o = lua_absindex(L,1), d = lua_absindex(L,2);
    float ox=fieldNum(L,o,"x",0), oy=fieldNum(L,o,"y",0), oz=fieldNum(L,o,"z",0);
    float dx=fieldNum(L,d,"x",0), dy=fieldNum(L,d,"y",0), dz=fieldNum(L,d,"z",0);
    float maxDist = lua_isnoneornil(L,3) ? 1000.0f : (float)luaL_checknumber(L,3);
    RaycastHit r = h->raycast(ox,oy,oz,dx,dy,dz,maxDist);
    lua_createtable(L, 0, 5);
    lua_pushboolean(L, r.hit); lua_setfield(L, -2, "hit");
    if (r.hit) {
        lua_pushnumber(L, r.distance);                       lua_setfield(L, -2, "distance");
        pushVec3(L, r.point[0],  r.point[1],  r.point[2]);   lua_setfield(L, -2, "point");
        pushVec3(L, r.normal[0], r.normal[1], r.normal[2]);  lua_setfield(L, -2, "normal");
        if (r.entity) pushEntity(L, h->world()->entity((flecs::entity_t)r.entity), h);
        else          lua_pushnil(L);
        lua_setfield(L, -2, "entity");
    }
    return 1;
}

// ── install ──────────────────────────────────────────────────────────────
inline void install(lua_State* L, ScriptHost* h) {
    // Entity metatable (methods resolve via __index = metatable)
    luaL_newmetatable(L, kEntityMT);
    lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
    static const luaL_Reg kEntity[] = {
        {"getTransform", e_getTransform}, {"setTransform", e_setTransform},
        {"isAlive", e_isAlive}, {"name", e_name}, {"destroy", e_destroy},
        {"setParent", e_setParent}, {"clearParent", e_clearParent},
        {"applyImpulse", e_applyImpulse}, {"setVelocity", e_setVelocity},
        {"move", e_move}, {"jump", e_jump}, {"isGrounded", e_isGrounded},
        {nullptr, nullptr}
    };
    lua_pushlightuserdata(L, h);
    luaL_setfuncs(L, kEntity, 1);
    lua_pop(L, 1); // metatable

    auto table = [&](const char* gname, const luaL_Reg* fns) {
        lua_newtable(L);
        lua_pushlightuserdata(L, h);
        luaL_setfuncs(L, fns, 1);
        lua_setglobal(L, gname);
    };
    static const luaL_Reg kInput[] = {{"keyDown",in_keyDown},{"keyPressed",in_keyPressed},
        {"axis",in_axis},{"mouseDown",in_mouseDown},{nullptr,nullptr}};
    static const luaL_Reg kLog[]   = {{"info",log_info},{"warn",log_warn},{"error",log_error},{nullptr,nullptr}};
    static const luaL_Reg kTime[]  = {{"dt",time_dt},{"elapsed",time_elapsed},{"frame",time_frame},{nullptr,nullptr}};
    static const luaL_Reg kWorld[] = {{"find",world_find},{"create",world_create},{nullptr,nullptr}};
    static const luaL_Reg kAudio[] = {{"play",audio_play},{"playAt",audio_playAt},{nullptr,nullptr}};
    static const luaL_Reg kPhysics[] = {{"raycast",phys_raycast},{nullptr,nullptr}};
    table("Input", kInput); table("Log", kLog); table("Time", kTime);
    table("World", kWorld); table("Audio", kAudio); table("Physics", kPhysics);
}

} // namespace LuaBindings
