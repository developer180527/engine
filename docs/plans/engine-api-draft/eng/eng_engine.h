/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 *
 * eng_engine.h: create the engine and advance it. Your program owns the loop:
 * engStep does one frame and returns. It never pumps OS events and never waits
 * for a window to come back from being minimised. */
#ifndef ENG_ENGINE_H
#define ENG_ENGINE_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

typedef struct EngModuleDesc EngModuleDesc; /* eng_module.h */

/* Picks a provider for a kind, overriding the project file.
 * Nested in EngEngineDesc as an array, so it can never grow. */
typedef struct EngProviderChoice {
    const char* kind;  /* "renderer", "physics", "audio", ... */
    const char* name;  /* "aaa", "jolt", "null", ... Unknown: refused by name. */
    uint64_t reserved[2]; /* must be zero */
} EngProviderChoice;

typedef uint32_t EngPacing;
enum {
    ENG_PACING_ENGINE = 0, /* engStep paces itself to the display (WO-061) */
    ENG_PACING_HOST = 1    /* engStep never sleeps: a display link, rAF or a
                              host toolkit decides when a frame happens */
};

typedef uint32_t EngLogLevel;
enum {
    ENG_LOG_TRACE = 0, ENG_LOG_DEBUG, ENG_LOG_INFO,
    ENG_LOG_SUCCESS, ENG_LOG_WARN, ENG_LOG_ERROR
};
typedef void (*EngLogFn)(void* user, EngLogLevel level,
                         const char* category, const char* message);

typedef struct EngEngineDesc {
    uint32_t struct_size;
    uint32_t api_version;       /* set to ENG_API_VERSION; 0 is refused */
    const char* app_name;       /* NULL: "engine" */
    const char* project_path;   /* NULL: code-first, no project file needed */
    const EngProviderChoice* providers;
    uint32_t provider_count;
    EngPacing pacing;
    /* Kits and providers linked into this program. iOS, consoles and the web
     * can only use this path; desktop may also use engLoadModule. */
    const EngModuleDesc* const* modules;
    uint32_t module_count;
    uint32_t reserved0;         /* must be zero */
    EngLogFn log_fn;            /* NULL: the default sink */
    void* log_user;
} EngEngineDesc;

/* Refuses, by name, an unknown provider or a missing required module. Call
 * engine functions from this thread unless a comment says otherwise. */
ENG_API EngResult engCreateEngine(const EngEngineDesc* desc, EngEngine** out);
ENG_API void engDestroyEngine(EngEngine* engine);

/* False once engRequestQuit was called or the last automatic window closed. */
ENG_API EngBool engIsRunning(const EngEngine* engine);
ENG_API void engRequestQuit(EngEngine* engine);

/* OS events: gather them explicitly. Neither call exists on a platform whose
 * OS delivers events itself; there they return ENG_ERROR_UNSUPPORTED. */
ENG_API EngResult engPumpEvents(EngEngine* engine);
ENG_API EngResult engWaitEvents(EngEngine* engine, double timeout_seconds);

typedef struct EngStepInfo {
    uint32_t struct_size;       /* set by the caller; written only up to this */
    uint32_t simulation_steps;  /* fixed steps run during this frame */
    uint64_t frame;
    double delta_seconds;
    float interpolation_alpha;
    uint32_t windows_presented; /* 0 when everything is minimised or hidden */
} EngStepInfo;

/* One frame: fixed-step simulation, frame systems, then rendering each view.
 * out may be NULL. */
ENG_API EngResult engStep(EngEngine* engine, EngStepInfo* out);
/* The same, with your own clock: tests, tools, offline capture. */
ENG_API EngResult engStepWithDelta(EngEngine* engine, double delta_seconds,
                                   EngStepInfo* out);

/* Convenience: pump, frame_fn, step, until quit. It blocks, so it returns
 * ENG_ERROR_UNSUPPORTED where a loop may not block (web, iOS): call engStep
 * from requestAnimationFrame or a display link there. */
typedef void (*EngFrameFn)(void* user, EngEngine* engine);
ENG_API EngResult engRun(EngEngine* engine, EngFrameFn frame_fn, void* user);

/* ── Log ── */
ENG_API void engLog(EngEngine* engine, EngLogLevel level,
                    const char* category, const char* message);

/* ── Memory: visible to budgets and leak accounting. ── */
ENG_API void* engAlloc(size_t size, size_t align, uint32_t tag);
ENG_API void engFree(void* pointer);
/* Valid until the end of this frame; never freed. NULL past the arena, which
 * is a normal outcome to handle. */
ENG_API void* engFrameAlloc(EngEngine* engine, size_t size, size_t align);

/* ── Jobs: the engine's pool. Do not start your own threads. ── */
typedef void (*EngRangeFn)(void* user, uint32_t begin, uint32_t end);
typedef void (*EngMainFn)(void* user);
ENG_API uint32_t engGetWorkerCount(EngEngine* engine);
/* Blocks until finished. fn may run on several threads at once. */
ENG_API void engParallelFor(EngEngine* engine, const char* name, uint32_t count,
                            uint32_t grain, EngRangeFn fn, void* user);
/* Any thread. Runs on the main thread during the next step. */
ENG_API void engRunOnMain(EngEngine* engine, EngMainFn fn, void* user);

/* ── Crash reports: key and value are copied and appear in the report. ── */
ENG_API void engSetCrashAnnotation(EngEngine* engine, const char* key,
                                   const char* value);

ENG_EXTERN_C_END
#endif /* ENG_ENGINE_H */
