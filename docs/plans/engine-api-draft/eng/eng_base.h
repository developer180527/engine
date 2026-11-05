/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 * See docs/plans/engine-api.md. The tag comes off at version 1.0.
 *
 * eng_base.h: versions, results, value types, and the rules every eng header
 * follows. C99 and C++, with no other dependency. */
#ifndef ENG_BASE_H
#define ENG_BASE_H

#include <stddef.h>
#include <stdint.h>

#define ENG_API_DRAFT 1

/* Same major: old kits and old programs keep working, in source and in binary.
 * While major is 0, nothing is promised and any minor may break. */
#define ENG_API_VERSION_MAJOR 0
#define ENG_API_VERSION_MINOR 1
#define ENG_API_VERSION ((ENG_API_VERSION_MAJOR << 16) | ENG_API_VERSION_MINOR)

#if defined(ENG_STATIC)
#  define ENG_API
#elif defined(_WIN32)
#  if defined(ENG_BUILDING)
#    define ENG_API __declspec(dllexport)
#  else
#    define ENG_API __declspec(dllimport)
#  endif
#elif defined(__EMSCRIPTEN__) || defined(__GNUC__)
#  define ENG_API __attribute__((visibility("default")))
#else
#  define ENG_API
#endif

#ifdef __cplusplus
#  define ENG_EXTERN_C_BEGIN extern "C" {
#  define ENG_EXTERN_C_END }
#else
#  define ENG_EXTERN_C_BEGIN
#  define ENG_EXTERN_C_END
#endif

ENG_EXTERN_C_BEGIN

/* A 32-bit boolean: one size in every language, on every target. */
typedef uint32_t EngBool;
#define ENG_FALSE 0u
#define ENG_TRUE 1u

/* Every fallible call returns this. New values are only ever appended, so
 * treat any value you do not know as a failure. */
typedef int32_t EngResult;
enum {
    ENG_OK = 0,
    ENG_NOT_READY = 1,              /* not a failure: still loading, ask later */
    ENG_ERROR_INVALID_ARGUMENT = -1,
    ENG_ERROR_ABI_MISMATCH = -2,    /* built against an incompatible version */
    ENG_ERROR_UNSUPPORTED = -3,     /* this platform or build cannot do it */
    ENG_ERROR_NOT_FOUND = -4,
    ENG_ERROR_STALE_HANDLE = -5,    /* it existed, and has since been destroyed */
    ENG_ERROR_WRONG_THREAD = -6,
    ENG_ERROR_BUFFER_TOO_SMALL = -7,/* *size now holds what is needed */
    ENG_ERROR_OUT_OF_MEMORY = -8,
    ENG_ERROR_REFUSED = -9,         /* e.g. a component layout disagrees */
    ENG_ERROR_DEVICE_LOST = -10,
    ENG_ERROR_INTERNAL = -11
};

/* Why this thread's last call failed, naming what it can. Thread-local; valid
 * until this thread's next failing call. Never NULL; never freed by the caller. */
ENG_API const char* engGetLastErrorMessage(void);

/* The running library's ENG_API_VERSION, which may be newer than the header. */
ENG_API uint32_t engGetApiVersion(void);

/* Starts a growable struct: every field but struct_size is zero, and zero is
 * always the default.   EngWindowDesc d = ENG_INIT(EngWindowDesc); */
#ifdef __cplusplus
#  define ENG_INIT(T) ([] { T v{}; v.struct_size = (uint32_t)sizeof(T); return v; }())
#else
#  define ENG_INIT(T) { .struct_size = (uint32_t)sizeof(T) }
#endif

/* Frozen value types: no struct_size, and they can never grow. */
typedef struct EngVec3 { float x, y, z; } EngVec3;
typedef struct EngQuat { float x, y, z, w; } EngQuat;
typedef struct EngTransform {
    EngVec3 position;
    EngQuat rotation;
    EngVec3 scale;
} EngTransform;

/* Objects: opaque pointers, each owned by whoever created it. */
typedef struct EngEngine EngEngine;
typedef struct EngWorld EngWorld;

/* Handles: plain values with a generation, so a stale one is refused rather
 * than crashing. Zero always means "none". An EngEntity is valid for this
 * session only; save the stable id instead (engGetEntityStableId). */
typedef uint64_t EngEntity;
typedef uint32_t EngWindow;
typedef uint32_t EngMonitor;
typedef uint32_t EngView;
typedef uint32_t EngOffscreen;
typedef uint32_t EngComponent;
typedef uint32_t EngQuery;
typedef uint32_t EngSystem;
typedef uint32_t EngChangeStream;
typedef uint32_t EngAction;
typedef uint32_t EngAsset;
/* Engine-owned variable-length bytes held by a component field
 * (ENG_FIELD_BLOB, eng_world.h). 64-bit: index and generation. */
typedef uint64_t EngBlob;

ENG_EXTERN_C_END
#endif /* ENG_BASE_H */
