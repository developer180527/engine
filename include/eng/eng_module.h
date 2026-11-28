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
 * Not in this version, and refused by name: services (requires_service_count
 * must be 0), and loading a module from a file (engLoadModule). Kits built as
 * C++ plugins still load from the project's manifest, as before. */
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

ENG_EXTERN_C_END
#endif /* ENG_MODULE_H */
