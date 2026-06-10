#pragma once
// ── Game module contract — C++ hot reload ───────────────────────────────────
// A game compiles to a shared library; engine_host loads it, adapts it into
// the plugin registry, and reloads the library whenever it changes on disk.
// All world state (entities, components, assets) lives in the HOST — it
// survives reloads. Module-internal state does not: keep state in
// components, keep logic in the module.
//
//   class MyGame final : public IEnginePlugin { ... };   // module-internal
//   ENGINE_GAME_MODULE(MyGame)
//
// THE BOUNDARY IS C. No C++ type crosses it: the module exports a plain
// function-pointer table (EngineGameModuleV1) — your IEnginePlugin class and
// its vtable stay entirely inside the module; the host wraps the table in a
// host-side adapter. This removes the vtable/RTTI/exception-ABI class of
// hot-reload bugs by construction, and structurally prevents STL containers,
// lambdas or polymorphic types from leaking into the contract.
//
// Reload cycle (sim keeps running, world persists):
//   old: onSimulationStop -> onDetach -> destroy -> dlclose
//   new: dlopen -> create -> onAttach -> onSimulationStart(same world)
//
// Engine services from inside a module: the C API (engine/engine_api.h) —
// those calls execute host-side. Header-inline singletons (Input::,
// InputSystem::get()) duplicate across the dylib boundary; never use them.
#include "runtime/plugin.h"           // module-internal convenience interface
#include "runtime/runtime_context.h"
#include <flecs.h>
#include <stdint.h>

// Shared component headers — these define the layout hash below.
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh.h"
#include "components/animator.h"
#include "components/camera.h"
#include "components/light.h"
#include "components/rigid_body.h"
#include "components/character_controller.h"
#include "components/script_component.h"
#include "components/collision_events.h"
#include "components/entity_id.h"

// ── Compatibility checks (all enforced by the host at load) ─────────────────
//  1. structSize       — exact table size for this version
//  2. apiVersion       — bumped when the table/contract changes shape
//  3. abiFingerprint   — compiler + C++ std + build mode, baked at compile
//                        time; catches debug-vs-release and cross-compiler
//  4. componentLayoutHash — sizeof/alignof of every shared component struct
//                        (and RuntimeContext). World data SURVIVES reloads,
//                        so a module compiled against changed component
//                        layouts would misread live ECS memory; the hash
//                        turns that silent corruption into a refusal with
//                        a "restart the host" message.
#define ENGINE_GAME_API_VERSION 2

#ifdef NDEBUG
    #define ENGINE_ABI_BUILD_MODE "release"
#else
    #define ENGINE_ABI_BUILD_MODE "debug"
#endif
#define ENGINE_ABI_STR2(x) #x
#define ENGINE_ABI_STR(x)  ENGINE_ABI_STR2(x)
#define ENGINE_ABI_FINGERPRINT \
    __VERSION__ "|c++" ENGINE_ABI_STR(__cplusplus) \
    "|api" ENGINE_ABI_STR(ENGINE_GAME_API_VERSION) "|" ENGINE_ABI_BUILD_MODE

namespace engine_abi {
constexpr uint64_t kFnvBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * kFnvPrime; }

constexpr uint64_t componentLayoutHash() {
    uint64_t h = kFnvBasis;
#define ENGINE_ABI_HASH_TYPE(T) h = mix(mix(h, sizeof(T)), alignof(T))
    ENGINE_ABI_HASH_TYPE(Transform);
    ENGINE_ABI_HASH_TYPE(Name);
    ENGINE_ABI_HASH_TYPE(MeshRenderer);
    ENGINE_ABI_HASH_TYPE(SkinnedMesh);
    ENGINE_ABI_HASH_TYPE(Animator);
    ENGINE_ABI_HASH_TYPE(Camera);
    ENGINE_ABI_HASH_TYPE(Light);
    ENGINE_ABI_HASH_TYPE(RigidBody);
    ENGINE_ABI_HASH_TYPE(CharacterController);
    ENGINE_ABI_HASH_TYPE(ScriptComponent);
    ENGINE_ABI_HASH_TYPE(CollisionEvents);
    ENGINE_ABI_HASH_TYPE(EntityId);
    ENGINE_ABI_HASH_TYPE(RuntimeContext);
#undef ENGINE_ABI_HASH_TYPE
    return h;
}
} // namespace engine_abi

// Why the host loaded the module — delivered via the (nullable) loadReason
// table entry, so a module can distinguish a cold start from a live swap.
typedef enum EngineModuleLoadReason {
    ENGINE_MODULE_LOAD_INITIAL   = 0,
    ENGINE_MODULE_LOAD_HOTRELOAD = 1,
} EngineModuleLoadReason;

// ── The boundary table — C types ONLY ────────────────────────────────────────
// Function pointers may be null (host checks every call). `user` is the
// module's instance; creation/destruction both happen module-side so
// allocators match — the host never deletes anything from this table.
// Evolution: V2 appends a new struct + new create symbol; V1 keeps working.
extern "C" {
typedef struct EngineGameModuleV1 {
    uint32_t    structSize;          // = sizeof(EngineGameModuleV1)
    uint32_t    apiVersion;          // = ENGINE_GAME_API_VERSION
    const char* abiFingerprint;      // = ENGINE_ABI_FINGERPRINT
    uint64_t    componentLayoutHash; // = engine_abi::componentLayoutHash()
    const char* name;
    const char* version;
    void*       user;

    void (*attach)     (void* user, void* runtimeContext /*RuntimeContext*/);
    void (*detach)     (void* user);
    void (*simStart)   (void* user, ecs_world_t* world);
    void (*simStop)    (void* user);
    void (*update)     (void* user, ecs_world_t* world, float dt);
    void (*physicsStep)(void* user, ecs_world_t* world, float dt);
    void (*postPhysics)(void* user, ecs_world_t* world);
    void (*loadReason) (void* user, int reason);   // nullable, optional
} EngineGameModuleV1;

typedef EngineGameModuleV1* (*EngineGameModuleCreateV1Fn)(void);
typedef void                (*EngineGameModuleDestroyV1Fn)(EngineGameModuleV1*);
}

// ── ENGINE_GAME_MODULE — place once in the module's main TU ─────────────────
// Generates the C table from an IEnginePlugin-derived class. The class and
// its vtable never cross the boundary; only these thunks do.
// Naming convention: engineGameModule* exports are reserved for game modules;
// future module kinds get their own prefix (engineEditorModule*, ...).
#define ENGINE_GAME_MODULE(PluginType)                                         \
    static void _egm_attach(void* u, void* c) {                                \
        static_cast<PluginType*>(u)->onAttach(                                 \
            *static_cast<RuntimeContext*>(c));                                 \
    }                                                                          \
    static void _egm_detach(void* u) {                                         \
        static_cast<PluginType*>(u)->onDetach();                               \
    }                                                                          \
    static void _egm_simStart(void* u, ecs_world_t* w) {                       \
        flecs::world wv(w);                                                    \
        static_cast<PluginType*>(u)->onSimulationStart(wv);                    \
    }                                                                          \
    static void _egm_simStop(void* u) {                                        \
        static_cast<PluginType*>(u)->onSimulationStop();                       \
    }                                                                          \
    static void _egm_update(void* u, ecs_world_t* w, float dt) {               \
        flecs::world wv(w);                                                    \
        static_cast<PluginType*>(u)->onUpdate(wv, dt);                         \
    }                                                                          \
    static void _egm_physics(void* u, ecs_world_t* w, float dt) {              \
        flecs::world wv(w);                                                    \
        static_cast<PluginType*>(u)->onPhysicsStep(wv, dt);                    \
    }                                                                          \
    static void _egm_post(void* u, ecs_world_t* w) {                           \
        flecs::world wv(w);                                                    \
        static_cast<PluginType*>(u)->onPostPhysics(wv);                        \
    }                                                                          \
    extern "C" __attribute__((visibility("default")))                          \
    EngineGameModuleV1* engineGameModuleCreateV1(void) {                       \
        auto* p = new PluginType();                                            \
        auto* t = new EngineGameModuleV1{};                                    \
        t->structSize          = sizeof(EngineGameModuleV1);                   \
        t->apiVersion          = ENGINE_GAME_API_VERSION;                      \
        t->abiFingerprint      = ENGINE_ABI_FINGERPRINT;                       \
        t->componentLayoutHash = engine_abi::componentLayoutHash();            \
        t->name        = p->name();                                            \
        t->version     = p->version();                                         \
        t->user        = p;                                                    \
        t->attach      = _egm_attach;                                          \
        t->detach      = _egm_detach;                                          \
        t->simStart    = _egm_simStart;                                        \
        t->simStop     = _egm_simStop;                                         \
        t->update      = _egm_update;                                          \
        t->physicsStep = _egm_physics;                                         \
        t->postPhysics = _egm_post;                                            \
        t->loadReason  = nullptr; /* fill in a custom create if you need it */ \
        return t;                                                              \
    }                                                                          \
    extern "C" __attribute__((visibility("default")))                          \
    void engineGameModuleDestroyV1(EngineGameModuleV1* t) {                    \
        if (!t) return;                                                        \
        delete static_cast<PluginType*>(t->user);                              \
        delete t;                                                              \
    }

// ── Design notes & accepted risks ────────────────────────────────────────────
// Eliminated by construction:
//   • C++ vtable/RTTI/exception ABI across the boundary  (C table only)
//   • STL/lambda/polymorphic types in the contract        (C types only)
//   • host deleting module memory                         (destroy fn only)
//   • optional-export configuration matrix                (nullable fields
//     in ONE struct; structSize gates additive evolution)
// Detected and refused at load:
//   • debug/release & cross-compiler mixes                (fingerprint)
//   • shared component/RuntimeContext layout drift        (layout hash)
// Still inherent — discipline, not mechanism:
//   • module globals/statics/threads/callbacks outliving dlclose: release
//     everything module-pointing in onDetach/onSimulationStop
//   • layout hash sees sizeof/alignof, not field order: same-size field
//     swaps slip through (rare; restart when editing components anyway)
//   • the fingerprint is a heuristic: it cannot see every ABI-relevant
//     flag (sanitizers, packing pragmas, stdlib variants), and identical
//     toolchains can still differ semantically
//   • code swaps under live data: the new code may hold assumptions the
//     old world never satisfied — semantic skew is undetectable in general
//   • this is a DEV-TIME contract for a cooperative build environment,
//     not a stable public ABI; shipped games link engine::runtime
//     statically and never dlopen anything
