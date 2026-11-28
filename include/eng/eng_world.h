/* UNSTABLE (0.x): see eng_base.h.
 *
 * eng_world.h: entities, components, queries, blobs, systems and the change
 * stream. flecs lives behind this and is named nowhere. A component is plain
 * bytes with a registered schema: no constructors, no pointers. Refer to
 * other entities by handle, and to variable-length bytes by blob. The design
 * is docs/plans/engine-api-draft/eng/eng_world.h; this header holds what is
 * implemented (WO-073). */
#ifndef ENG_WORLD_H
#define ENG_WORLD_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

/* This version has one world: the engine's simulation world, which lives as
 * long as the engine. engCreateWorld starts the engine headless if it has not
 * started, and a second engCreateWorld while one is held is
 * ENG_ERROR_UNSUPPORTED. engDestroyWorld releases the handle: the queries,
 * systems and change streams made through it end, and the world's entities
 * (and their blobs) stay. */
typedef struct EngWorldDesc {
    uint32_t struct_size;
    uint32_t reserved;          /* must be zero */
    double fixed_step_seconds;  /* 0: 1/60, the only step this version runs */
} EngWorldDesc;
ENG_API EngResult engCreateWorld(EngEngine* engine, const EngWorldDesc* desc, EngWorld** out);
/* Its systems end with it; refused while a query iteration is open, or from
 * inside a system. */
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
    /* An EngBlob: variable-length bytes the engine owns (a list of contacts, a
     * path, a spline's points). count must be 0 or 1. See "Blobs" below. */
    ENG_FIELD_BLOB
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

/* ── Blobs: the bytes behind an ENG_FIELD_BLOB field ──
 * A blob field holds an EngBlob, or 0 for no bytes. The component stays plain
 * bytes; the blob is saved and loaded with the scene (base64 on disk), hashed
 * by its bytes, and freed with the field that holds it.
 *
 * Ownership. engCreateBlob makes a blob nobody holds yet. Storing its handle
 * in a blob field (engAddComponent, engSetComponent, or a WRITE column of a
 * query) gives it to that field. Storing a handle another field already holds
 * stores a COPY: the engine writes a new handle into the field, so two fields
 * never share bytes. A held blob is freed when its field stops holding it:
 * the field is set to another handle or 0, the component is removed, or the
 * entity is destroyed. A blob never stored is freed with the world.
 *
 * A stale handle (freed) is ENG_ERROR_STALE_HANDLE; a new blob never reuses
 * one. A blob is at most 16 MiB in this version. */
ENG_API EngResult engCreateBlob(EngWorld* world, const void* bytes_or_null, uint32_t size, EngBlob* out);
/* *out_bytes is valid until the blob is next set or freed; NULL when size is 0. */
ENG_API EngResult engGetBlob(EngWorld* world, EngBlob blob, const void** out_bytes, uint32_t* out_size);
/* Replaces the bytes, keeping the handle. Counts as a change of the component
 * that holds it. */
ENG_API EngResult engSetBlob(EngWorld* world, EngBlob blob, const void* bytes_or_null, uint32_t size);

/* ── Systems: your code in the engine's frame ──
 * A system runs only while the simulation runs (engStep starts it).
 *   FIXED_UPDATE  each simulation step, at 1/60 s, where kits' onUpdate runs:
 *                 before physics, deterministic, replayed. The phase that
 *                 writes simulation state.
 *   UPDATE        once per frame, after the frame's steps, before any kit's
 *                 onFrame. delta_seconds is the frame's.
 *   LATE_UPDATE   once per frame after every kit's onFrame, just before
 *                 rendering reads the world.
 * Within a phase, systems run in the order they were created, moved only as
 * run_after requires. */
typedef uint32_t EngPhase;
enum {
    ENG_PHASE_FIXED_UPDATE = 0,
    ENG_PHASE_UPDATE = 1,
    ENG_PHASE_LATE_UPDATE = 2
};
typedef struct EngSystemContext {
    uint32_t struct_size;
    uint32_t reserved;     /* zero */
    double delta_seconds;
    uint64_t tick;         /* simulation steps taken, this one included in FIXED_UPDATE */
    EngWorld* world;
    void* user;
    /* With a query: the iteration the batch comes from, for engMarkChanged.
     * Without one: NULL. */
    EngQueryIter* iter;
} EngSystemContext;
/* With a query: called once per batch (never for an empty one). Without one:
 * called once, batch NULL. */
typedef void (*EngSystemFn)(const EngSystemContext* context, const EngQueryBatch* batch);

typedef struct EngSystemDesc {
    uint32_t struct_size;
    EngPhase phase;
    const char* name;      /* namespaced and unique: "apollo.descent_engine" */
    EngQuery query;        /* 0: none. Not destroyable while a system uses it */
    EngBool parallel;      /* not in this version: ENG_TRUE is refused by name */
    EngSystemFn fn;
    void* user;
    /* Names of systems in the same phase that must run first. A name not
     * created yet counts once it is. A cycle, or a name in another phase, is
     * refused by name, whichever system comes second. */
    const char* const* run_after;
    uint32_t run_after_count;
    uint32_t reserved;     /* must be zero */
} EngSystemDesc;
ENG_API EngResult engCreateSystem(EngWorld* world, const EngSystemDesc* desc, EngSystem* out);
/* Also from inside a system: it does not run again. */
ENG_API void engDestroySystem(EngWorld* world, EngSystem system);

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
