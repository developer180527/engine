/* UNSTABLE (0.x): see eng_base.h.
 *
 * eng_world.h: entities, components, queries and the change stream. flecs
 * lives behind this and is named nowhere. A component is plain bytes with a
 * registered schema: no constructors, no pointers. Refer to other entities by
 * handle. The design is docs/plans/engine-api-draft/eng/eng_world.h; this
 * header holds what is implemented (WO-073, its first part). Systems and
 * blobs are declared when they exist. */
#ifndef ENG_WORLD_H
#define ENG_WORLD_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

/* This version has one world: the engine's simulation world, which lives as
 * long as the engine. engCreateWorld starts the engine headless if it has not
 * started, and a second engCreateWorld while one is held is
 * ENG_ERROR_UNSUPPORTED. engDestroyWorld releases the handle: the queries and
 * change streams made through it end, and the world's entities stay. */
typedef struct EngWorldDesc {
    uint32_t struct_size;
    uint32_t reserved;          /* must be zero */
    double fixed_step_seconds;  /* 0: 1/60, the only step this version runs */
} EngWorldDesc;
ENG_API EngResult engCreateWorld(EngEngine* engine, const EngWorldDesc* desc, EngWorld** out);
ENG_API void engDestroyWorld(EngWorld* world);

/* ── Entities ──
 * An EngEntity carries a generation: once the entity is destroyed its handle
 * is refused (ENG_ERROR_STALE_HANDLE), and a new entity never has it. */
ENG_API EngResult engCreateEntity(EngWorld* world, const char* name_or_null, EngEntity* out);
ENG_API void engDestroyEntity(EngWorld* world, EngEntity entity);
ENG_API EngBool engIsEntityAlive(EngWorld* world, EngEntity entity);
/* The first entity with this name, or 0. A scan: for one-off lookups. */
ENG_API EngEntity engFindEntity(EngWorld* world, const char* name);
/* parent_or_0 = 0 clears the parent. A cycle, or a chain too deep, is refused. */
ENG_API EngResult engSetEntityParent(EngWorld* world, EngEntity child, EngEntity parent_or_0);
ENG_API EngEntity engGetEntityParent(EngWorld* world, EngEntity child);
/* What save files refer to. It survives sessions; an EngEntity does not.
 * id[1] is zero in this version (the stable id is 64 bits). */
ENG_API EngResult engGetEntityStableId(EngWorld* world, EngEntity entity, uint64_t out[2]);
/* A scan: for one-off lookups. */
ENG_API EngEntity engFindEntityByStableId(EngWorld* world, const uint64_t id[2]);

/* ── Component schemas ── */
typedef uint32_t EngFieldType;
enum {
    ENG_FIELD_BOOL32 = 0, ENG_FIELD_I32, ENG_FIELD_U32, ENG_FIELD_I64, ENG_FIELD_U64,
    ENG_FIELD_F32, ENG_FIELD_F64, ENG_FIELD_VEC3, ENG_FIELD_QUAT,
    ENG_FIELD_ENTITY,          /* an EngEntity; saved as the entity's stable id */
    ENG_FIELD_ASSET,           /* not in this version: refused at registration */
    ENG_FIELD_FIXED_STRING,    /* count bytes of NUL-terminated UTF-8 */
    /* Appended, never reordered. */
    ENG_FIELD_U8, ENG_FIELD_I8, ENG_FIELD_U16, ENG_FIELD_I16,
    ENG_FIELD_VEC2, ENG_FIELD_VEC4, /* VEC4 also carries colours */
    ENG_FIELD_MAT4,                 /* 16 floats, column-major */
    ENG_FIELD_BLOB                  /* not in this version: refused at registration */
};
/* Passed as an array, so it can never grow. */
typedef struct EngFieldDesc {
    const char* name;
    EngFieldType type;
    uint32_t offset;
    uint32_t count;    /* 0 or 1: one value; otherwise an array of count values */
    uint32_t reserved; /* must be zero */
} EngFieldDesc;

typedef struct EngComponentDesc {
    uint32_t struct_size;
    uint32_t revision;  /* bump when the same bytes start to mean something else */
    const char* name;   /* namespaced: "apollo.Thruster". Saved under this name. */
    uint32_t size;
    uint32_t align;
    const EngFieldDesc* fields;
    uint32_t field_count;
    uint32_t reserved0; /* must be zero */
    const void* defaults; /* size bytes used by engAddComponent; NULL: all zero */
    /* Not in this version: set, it is refused by name. Data saved under an
     * older revision is then not loaded, and the load says so. */
    EngResult (*migrate)(uint32_t from_revision, const void* old_bytes,
                         uint32_t old_size, void* new_bytes);
} EngComponentDesc;

/* The component is saved and loaded with scenes, field by field, under its
 * name. Registering again with an identical schema returns the same id. A
 * different layout or revision is ENG_ERROR_REFUSED, and the message names
 * both sides. A name the engine owns ("core.*") is refused: use
 * engFindComponent. */
ENG_API EngResult engRegisterComponent(EngWorld* world, const EngComponentDesc* desc,
                                       EngComponent* out);
/* A registered component, or an engine one this version exposes as bytes:
 * "core.Transform" (EngTransform). 0 when there is none, with the reason in
 * engGetLastErrorMessage. */
ENG_API EngComponent engFindComponent(EngWorld* world, const char* name);

/* ── One entity at a time: simple, copying, and seen by the change stream ──
 * size must be the component's registered size. */
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
    ENG_ACCESS_READ = 0,   /* column supplied, read-only */
    ENG_ACCESS_WRITE = 1,  /* column supplied; every row in it counts as changed */
    ENG_ACCESS_WITH = 2,   /* must have it; no column */
    ENG_ACCESS_WITHOUT = 3,/* must not have it; no column */
    /* Column supplied, writable, but only the rows passed to engMarkChanged
     * count as changed: physics writing only awake bodies' transforms must not
     * make the renderer re-upload every sleeping one. */
    ENG_ACCESS_WRITE_MARKED = 4
};
/* Passed as an array, so it can never grow. */
typedef struct EngQueryTerm {
    EngComponent component;
    EngAccess access;
    uint32_t reserved[2]; /* must be zero */
} EngQueryTerm;

typedef struct EngQueryDesc {
    uint32_t struct_size;
    uint32_t term_count;  /* 1 to 16 */
    const EngQueryTerm* terms;
} EngQueryDesc;
ENG_API EngResult engCreateQuery(EngWorld* world, const EngQueryDesc* desc, EngQuery* out);
/* Refused (and kept) while one of its iterations is open. */
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
 * and applied at End. Every Begin needs its End, also after an early break. */
ENG_API EngResult engQueryBegin(EngWorld* world, EngQuery query, EngQueryIter** out);
ENG_API EngBool engQueryNext(EngQueryIter* iter, EngQueryBatch* out);
ENG_API void engQueryEnd(EngQueryIter* iter);
/* For an ENG_ACCESS_WRITE_MARKED term, rows [first, first + count) of the
 * current batch changed. Rows not marked are not reported. */
ENG_API void engMarkChanged(EngQueryIter* iter, uint32_t term, uint32_t first, uint32_t count);

/* ── The change stream: what changed since you last looked ──
 * Costs nothing for a component nobody subscribes to (DR-0011). A new stream
 * first reports every entity that already has the component, as ADDED.
 * Not between engQueryBegin and engQueryEnd. */
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
 * the capacity. Each entity appears at most once per stream between two
 * reads: changed many times is one CHANGED; added then changed is ADDED;
 * changed then removed is REMOVED; added then removed is nothing. An entity
 * id is not reused while a stream could still report it. */
ENG_API EngResult engReadChanges(EngWorld* world, EngChangeStream stream,
                                 EngChange* out, uint32_t* count);

ENG_EXTERN_C_END
#endif /* ENG_WORLD_H */
