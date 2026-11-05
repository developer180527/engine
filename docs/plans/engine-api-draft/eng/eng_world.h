/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 *
 * eng_world.h: entities, components, queries, systems and the change stream.
 * flecs lives behind this and is named nowhere. A component is plain bytes with
 * a registered schema: no constructors, no pointers. Refer to assets and other
 * entities by handle. */
#ifndef ENG_WORLD_H
#define ENG_WORLD_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

/* The first version may refuse a second world with ENG_ERROR_UNSUPPORTED. The
 * handle exists now so that allowing more later changes no signature. */
typedef struct EngWorldDesc {
    uint32_t struct_size;
    uint32_t reserved;     /* must be zero */
    double fixed_step_seconds; /* 0: 1/60 */
} EngWorldDesc;
ENG_API EngResult engCreateWorld(EngEngine* engine, const EngWorldDesc* desc, EngWorld** out);
ENG_API void engDestroyWorld(EngWorld* world);

/* ── Entities ── */
ENG_API EngResult engCreateEntity(EngWorld* world, const char* name_or_null, EngEntity* out);
ENG_API void engDestroyEntity(EngWorld* world, EngEntity entity);
ENG_API EngBool engIsEntityAlive(EngWorld* world, EngEntity entity);
ENG_API EngEntity engFindEntity(EngWorld* world, const char* name);
ENG_API EngResult engSetEntityParent(EngWorld* world, EngEntity child, EngEntity parent_or_0);
ENG_API EngEntity engGetEntityParent(EngWorld* world, EngEntity child);
/* What save files refer to. It survives sessions; an EngEntity does not. */
ENG_API EngResult engGetEntityStableId(EngWorld* world, EngEntity entity, uint64_t out[2]);
ENG_API EngEntity engFindEntityByStableId(EngWorld* world, const uint64_t id[2]);

/* ── Component schemas ── */
typedef uint32_t EngFieldType;
enum {
    ENG_FIELD_BOOL32 = 0, ENG_FIELD_I32, ENG_FIELD_U32, ENG_FIELD_I64, ENG_FIELD_U64,
    ENG_FIELD_F32, ENG_FIELD_F64, ENG_FIELD_VEC3, ENG_FIELD_QUAT,
    ENG_FIELD_ENTITY, ENG_FIELD_ASSET,
    ENG_FIELD_FIXED_STRING /* count bytes of NUL-terminated UTF-8 */
};
/* Passed as an array, so it can never grow. */
typedef struct EngFieldDesc {
    const char* name;
    EngFieldType type;
    uint32_t offset;
    uint32_t count;    /* 0 or 1: one value; otherwise an array */
    uint32_t reserved; /* must be zero */
} EngFieldDesc;

typedef struct EngComponentDesc {
    uint32_t struct_size;
    uint32_t revision;  /* bump when the same bytes start to mean something else */
    const char* name;   /* namespaced: "core.Transform", "apollo.Thruster" */
    uint32_t size;
    uint32_t align;
    const EngFieldDesc* fields;
    uint32_t field_count;
    uint32_t reserved0; /* must be zero */
    const void* defaults; /* size bytes used by engAddComponent; NULL: all zero */
    /* Upgrades data saved under an older revision. NULL: only this revision loads. */
    EngResult (*migrate)(uint32_t from_revision, const void* old_bytes,
                         uint32_t old_size, void* new_bytes);
} EngComponentDesc;

/* Registering again with an identical schema returns the same id. A different
 * layout or revision is ENG_ERROR_REFUSED, and the message names both sides:
 * this replaces today's compiler fingerprint and layout hash. */
ENG_API EngResult engRegisterComponent(EngWorld* world, const EngComponentDesc* desc,
                                       EngComponent* out);
ENG_API EngComponent engFindComponent(EngWorld* world, const char* name);

/* ── One entity at a time: simple, copying, marks changes for you ── */
ENG_API EngResult engAddComponent(EngWorld* world, EngEntity entity, EngComponent component,
                                  const void* initial_or_null);
ENG_API EngResult engRemoveComponent(EngWorld* world, EngEntity entity, EngComponent component);
ENG_API EngBool engHasComponent(EngWorld* world, EngEntity entity, EngComponent component);
ENG_API EngResult engGetComponent(EngWorld* world, EngEntity entity, EngComponent component,
                                  void* out, uint32_t size);
ENG_API EngResult engSetComponent(EngWorld* world, EngEntity entity, EngComponent component,
                                  const void* bytes, uint32_t size);

/* ── Queries: thousands at a time, in place. The fast path for every language ── */
typedef uint32_t EngAccess;
enum {
    ENG_ACCESS_READ = 0,  /* column supplied, read-only */
    ENG_ACCESS_WRITE = 1, /* column supplied; every row in it counts as changed */
    ENG_ACCESS_WITH = 2,  /* must have it; no column */
    ENG_ACCESS_WITHOUT = 3/* must not have it; no column */
};
/* Passed as an array, so it can never grow. */
typedef struct EngQueryTerm {
    EngComponent component;
    EngAccess access;
    uint32_t reserved[2]; /* must be zero */
} EngQueryTerm;

typedef struct EngQueryDesc {
    uint32_t struct_size;
    uint32_t term_count;
    const EngQueryTerm* terms;
} EngQueryDesc;
ENG_API EngResult engCreateQuery(EngWorld* world, const EngQueryDesc* desc, EngQuery* out);
ENG_API void engDestroyQuery(EngWorld* world, EngQuery query);

/* One run of rows stored together. columns[i] is an array of count values for
 * term i, or NULL for WITH and WITHOUT. All of it is valid only until the next
 * engQueryNext or engQueryEnd. In Python and JS these wrap straight into NumPy
 * arrays and typed arrays with no copy. */
typedef struct EngQueryBatch {
    uint32_t struct_size;  /* set by the caller */
    uint32_t count;
    const EngEntity* entities;
    void* const* columns;
} EngQueryBatch;

typedef struct EngQueryIter EngQueryIter;
/* Between Begin and End, adding or removing entities and components is queued
 * and applied at End. */
ENG_API EngResult engQueryBegin(EngWorld* world, EngQuery query, EngQueryIter** out);
ENG_API EngBool engQueryNext(EngQueryIter* iter, EngQueryBatch* out);
ENG_API void engQueryEnd(EngQueryIter* iter);

/* ── Systems: your code in the engine's frame ── */
typedef uint32_t EngPhase;
enum {
    ENG_PHASE_FIXED_UPDATE = 0, /* every simulation step: deterministic, replays */
    ENG_PHASE_UPDATE = 1,       /* every frame */
    ENG_PHASE_LATE_UPDATE = 2   /* every frame, just before rendering reads the world */
};
typedef struct EngSystemContext {
    uint32_t struct_size;
    uint32_t reserved;     /* must be zero */
    double delta_seconds;
    uint64_t tick;
    EngWorld* world;
    void* user;
} EngSystemContext;
/* With a query: called once per batch. Without one: called once, batch NULL. */
typedef void (*EngSystemFn)(const EngSystemContext* context, const EngQueryBatch* batch);

typedef struct EngSystemDesc {
    uint32_t struct_size;
    EngPhase phase;
    const char* name;
    EngQuery query;      /* 0: none */
    EngBool parallel;    /* batches may run at once, on the engine's pool */
    EngSystemFn fn;
    void* user;
    const char* const* run_after;  /* names of systems; a cycle is refused by name */
    uint32_t run_after_count;
    uint32_t reserved;   /* must be zero */
} EngSystemDesc;
ENG_API EngResult engCreateSystem(EngWorld* world, const EngSystemDesc* desc, EngSystem* out);
ENG_API void engDestroySystem(EngWorld* world, EngSystem system);

/* ── The change stream: what changed since you last looked ──
 * Costs nothing for a component nobody subscribes to (DR-0011). */
typedef uint32_t EngChangeKind;
enum { ENG_CHANGE_ADDED = 0, ENG_CHANGE_CHANGED = 1, ENG_CHANGE_REMOVED = 2 };
/* Returned as an array, so it can never grow. */
typedef struct EngChange {
    EngEntity entity;
    EngChangeKind kind;
    uint32_t reserved;  /* zero */
} EngChange;
ENG_API EngResult engSubscribeChanges(EngWorld* world, EngComponent component, EngChangeStream* out);
ENG_API void engUnsubscribeChanges(EngWorld* world, EngChangeStream stream);
/* count: in, capacity; out, entries written. Call until it returns fewer than
 * the capacity. */
ENG_API EngResult engReadChanges(EngWorld* world, EngChangeStream stream,
                                 EngChange* out, uint32_t* count);

ENG_EXTERN_C_END
#endif /* ENG_WORLD_H */
