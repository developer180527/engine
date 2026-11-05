/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 *
 * eng_module.h: kits and providers, and how they find each other.
 *
 * A module is ordinary code with one small description. It calls the same eng*
 * functions as a program, so nothing else is needed: no base class, no engine
 * headers, no flecs, no compiler match. Linked in or loaded, the source is the
 * same. Registration happens in on_load. */
#ifndef ENG_MODULE_H
#define ENG_MODULE_H
#include "eng_engine.h" /* EngModuleDesc is declared there */

ENG_EXTERN_C_BEGIN

typedef void (*EngProc)(void);
/* NULL for a function this engine does not have. The generated client stub
 * calls this for every function at load, so a module never links engine
 * symbols, and no table layout has to stay frozen. */
typedef EngProc (*EngGetProcFn)(const char* function_name);

struct EngModuleDesc {
    uint32_t struct_size;
    uint32_t api_version;     /* ENG_API_VERSION this was built against */
    const char* name;         /* "fps_player_controller" */
    const char* version;      /* the module's own, shown in errors */
    const char* const* requires_services; /* "engine.physics.v1"; refused by name if absent */
    uint32_t requires_service_count;
    uint32_t reserved;        /* must be zero */
    /* All optional. state is yours; the engine only passes it back. */
    EngResult (*on_load)(EngEngine* engine, void** state);
    void (*on_unload)(EngEngine* engine, void* state);
    EngResult (*on_world_start)(EngWorld* world, void* state);
    void (*on_world_stop)(EngWorld* world, void* state);
};

/* A loaded module exports this one function, and nothing else crosses. */
#define ENG_MODULE_ENTRY_NAME "engModuleEntry"
typedef const EngModuleDesc* (*EngModuleEntryFn)(EngGetProcFn get_proc);

/* Load by path, at development time only (hot reload lives here). A shipped game
 * returns ENG_ERROR_UNSUPPORTED: the mechanism is absent from its binary, and
 * its modules were linked in through EngEngineDesc.modules. */
ENG_API EngResult engLoadModule(EngEngine* engine, const char* path);

/* ── Services: named tables of functions, of any kind ──
 * How a provider offers physics, audio or something the engine has never heard
 * of, without the engine growing. A name carries its major version
 * ("voxel.world.v1"). A table starts with struct_size and only grows at its end;
 * a real break is a new name. */
typedef struct EngServiceDesc {
    uint32_t struct_size;
    uint32_t table_size;
    const char* name;
    const void* table;   /* must outlive the module */
} EngServiceDesc;
ENG_API EngResult engRegisterService(EngEngine* engine, const EngServiceDesc* desc);

/* min_table_size: normally sizeof your table. An older provider whose table is
 * smaller gives NOT_FOUND, so once you hold a table you never check a version
 * again. Absent is a normal answer: degrade, or declare it as required. */
ENG_API EngResult engFindService(EngEngine* engine, const char* name,
                                 uint32_t min_table_size, const void** out_table);

/* ── An example service: what gameplay sees of physics ── */
#define ENG_SERVICE_PHYSICS_V1 "engine.physics.v1"
typedef struct EngRaycastHit {
    EngEntity entity; /* 0: nothing was hit */
    EngVec3 point;
    EngVec3 normal;
    float distance;
    uint32_t reserved; /* zero */
} EngRaycastHit;
typedef struct EngPhysicsV1 {
    uint32_t struct_size;
    uint32_t reserved;
    void* self;       /* passed back as the first argument of each function */
    /* Optional extras, by name: has(self, "soft_bodies"). */
    EngBool (*has)(void* self, const char* capability);
    EngResult (*raycast)(void* self, EngWorld* world, EngVec3 origin, EngVec3 direction,
                         float max_distance, EngRaycastHit* out);
    EngResult (*apply_impulse)(void* self, EngWorld* world, EngEntity entity, EngVec3 impulse);
    EngResult (*set_velocity)(void* self, EngWorld* world, EngEntity entity, EngVec3 velocity);
    EngResult (*get_velocity)(void* self, EngWorld* world, EngEntity entity, EngVec3* out);
    EngResult (*teleport)(void* self, EngWorld* world, EngEntity entity, EngVec3 position);
} EngPhysicsV1;

ENG_EXTERN_C_END
#endif /* ENG_MODULE_H */
