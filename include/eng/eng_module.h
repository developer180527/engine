/* UNSTABLE (0.x): see eng_base.h.
 *
 * eng_module.h: kits, linked into the program.
 *
 * A module is ordinary code with one small description. It calls the same eng*
 * functions as a program, so it needs nothing else: no base class, no engine
 * C++ headers, no flecs. The program passes its modules in
 * EngEngineDesc.modules; the engine calls them back at four moments:
 *
 *   on_load         at engCreateEngine, in list order. Write *state; the
 *                   engine only passes it back.
 *   on_world_start  at engCreateWorld, in list order, once the world exists.
 *                   Register components and create systems here.
 *   on_world_stop   at engDestroyWorld (and engDestroyEngine, if a world is
 *                   still held), in reverse order, while the world still works.
 *   on_unload       at engDestroyEngine, in reverse order, last.
 *
 * All four are optional. A failing on_load or on_world_start fails the call
 * that ran it: the modules already started are stopped (or unloaded) in
 * reverse, and the error names the module. Components a module registered
 * before that stay registered; this version cannot unregister one.
 *
 * Two ways in, with the same source:
 *   linked in    the program lists the module in EngEngineDesc.modules.
 *   a project's  the module is a shared library named in project.json's
 *   kit          "kits", beside C++ kits. It exports one function,
 *                engModuleEntry, and reaches the engine only through the
 *                get_proc it is given: build it with ENG_CLIENT defined and
 *                eng_client.h implemented once, so it links nothing of the
 *                engine's. on_load gets engine NULL there (the host owns the
 *                frame), and the world is the simulation's, started at each
 *                Play and stopped at each Stop.
 *
 * Not in this version, and refused by name: services (requires_service_count
 * must be 0), and loading a module from a program (engLoadModule). */
#ifndef ENG_MODULE_H
#define ENG_MODULE_H
#include "eng_engine.h" /* EngModuleDesc is declared there */

ENG_EXTERN_C_BEGIN

/* The description, its strings and the callbacks must outlive the engine:
 * the engine keeps the pointer. A static const is the usual shape. */
struct EngModuleDesc {
    uint32_t struct_size;     /* sizeof(EngModuleDesc) */
    uint32_t api_version;     /* ENG_API_VERSION this was built against */
    const char* name;         /* "combat"; unique among the modules */
    const char* version;      /* the module's own, shown in errors; may be NULL */
    const char* const* requires_services; /* not in this version */
    uint32_t requires_service_count;      /* must be 0 in this version */
    uint32_t reserved;        /* must be zero */
    EngResult (*on_load)(EngEngine* engine, void** state);
    void (*on_unload)(EngEngine* engine, void* state);
    EngResult (*on_world_start)(EngWorld* world, void* state);
    void (*on_world_stop)(EngWorld* world, void* state);
};

/* ── Functions by name ──
 * NULL for a function this engine does not have. eng_client.h asks for every
 * function it forwards, so a module never resolves the host's symbols. */
typedef void (*EngProc)(void);
typedef EngProc (*EngGetProcFn)(const char* function_name);
ENG_API EngProc engGetProc(const char* function_name);

/* A module that is a shared library exports this, and nothing else crosses:
 *   ENG_MODULE_EXPORT const EngModuleDesc* engModuleEntry(EngGetProcFn get_proc);
 * NULL refuses the load (say, a function it needs is missing). */
#define ENG_MODULE_ENTRY_NAME "engModuleEntry"
typedef const EngModuleDesc* (*EngModuleEntryFn)(EngGetProcFn get_proc);
#if defined(_WIN32)
#  define ENG_MODULE_EXPORT ENG_EXTERN_C_DECL __declspec(dllexport)
#elif defined(__GNUC__)
#  define ENG_MODULE_EXPORT ENG_EXTERN_C_DECL __attribute__((visibility("default")))
#else
#  define ENG_MODULE_EXPORT ENG_EXTERN_C_DECL
#endif
#ifdef __cplusplus
#  define ENG_EXTERN_C_DECL extern "C"
#else
#  define ENG_EXTERN_C_DECL
#endif

ENG_EXTERN_C_END
#endif /* ENG_MODULE_H */
