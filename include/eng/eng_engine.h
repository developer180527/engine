/* UNSTABLE (0.x): see eng_base.h.
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
    ENG_PACING_ENGINE = 0, /* engRun may block (today: while the window is
                              minimised); WO-061 adds the frame limiter */
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
    const char* project_path;   /* NULL: code-first, no project file needed.
                                   Set: the project's cooked main scene is
                                   loaded and simulated, as engine_player does;
                                   a missing project.json or cooked scene is
                                   refused by name. */
    const EngProviderChoice* providers;
    uint32_t provider_count;
    EngPacing pacing;
    /* Kits and providers linked into this program. iOS, consoles and the web
     * can only use this path; desktop may also use engLoadModule. Not
     * implemented in this version: a non-zero count is refused by name. */
    const EngModuleDesc* const* modules;
    uint32_t module_count;
    uint32_t reserved0;         /* must be zero */
    EngLogFn log_fn;            /* NULL: the default sink. Not implemented in
                                   this version: set, it is refused by name. */
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

/* One frame: fixed-step simulation, frame systems, then rendering. It never
 * pumps events and never waits: while the window cannot be shown (minimised)
 * the simulation still steps and only rendering is skipped (DR-0030). Without
 * a window the first step starts the engine headless. out may be NULL. */
ENG_API EngResult engStep(EngEngine* engine, EngStepInfo* out);
/* The same, with your own clock: tests, tools, offline capture. */
ENG_API EngResult engStepWithDelta(EngEngine* engine, double delta_seconds,
                                   EngStepInfo* out);

/* Convenience: pump, frame_fn, step, until quit. It blocks, so it returns
 * ENG_ERROR_UNSUPPORTED where a loop may not block (web, iOS): call engStep
 * from requestAnimationFrame or a display link there. */
typedef void (*EngFrameFn)(void* user, EngEngine* engine);
ENG_API EngResult engRun(EngEngine* engine, EngFrameFn frame_fn, void* user);

/* Not implemented yet, so not declared here: engLog, engAlloc, engFree,
 * engFrameAlloc, engGetWorkerCount, engParallelFor, engRunOnMain,
 * engSetCrashAnnotation (docs/plans/engine-api-draft/eng/eng_engine.h). */

ENG_EXTERN_C_END
#endif /* ENG_ENGINE_H */
