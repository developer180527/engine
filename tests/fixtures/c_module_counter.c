/* c_module_counter: a C module for c_kit_host_test (WO-094).
 *
 * Its system adds one to counter.Count on the entity "counter" every
 * simulation step, and on_world_stop deliberately destroys nothing: the host
 * must remove what a module made, or a restarted (or reloaded) module would
 * count twice a step. on_world_stop and on_unload are counted on the same
 * entity's other fields, while the world still works. */
#define ENG_CLIENT_IMPLEMENTATION
#include <eng/eng_client.h>

typedef struct Count { uint32_t steps, starts, stops, pad; } Count;

static EngComponent g_count = 0;

static EngEntity counterEntity(EngWorld* w) {
    EngEntity e = engFindEntity(w, "counter");
    if (!e && engCreateEntity(w, "counter", &e) == ENG_OK) engAddComponent(w, e, g_count, NULL);
    return e;
}

static void bump(EngWorld* w, int field) {
    const EngEntity e = counterEntity(w);
    Count c;
    if (!e || engGetComponent(w, e, g_count, &c, (uint32_t)sizeof c) != ENG_OK) return;
    if (field == 0) ++c.steps;
    if (field == 1) ++c.starts;
    if (field == 2) ++c.stops;
    engSetComponent(w, e, g_count, &c, (uint32_t)sizeof c);
}

static void tick(const EngSystemContext* ctx, const EngQueryBatch* b) {
    (void)b;
    bump(ctx->world, 0);
}

static EngResult onWorldStart(EngWorld* w, void* state) {
    static const EngFieldDesc fields[] = {
        { "steps", ENG_FIELD_U32, 0, 1, 0 }, { "starts", ENG_FIELD_U32, 4, 1, 0 },
        { "stops", ENG_FIELD_U32, 8, 1, 0 }, { "pad", ENG_FIELD_U32, 12, 1, 0 },
    };
    EngComponentDesc d = ENG_INIT(EngComponentDesc);
    EngSystemDesc sd = ENG_INIT(EngSystemDesc);
    EngSystem s = 0;
    EngResult r;
    (void)state;
    d.name = "counter.Count";
    d.size = sizeof(Count);
    d.align = 4;
    d.fields = fields;
    d.field_count = 4;
    if ((r = engRegisterComponent(w, &d, &g_count)) != ENG_OK) return r;
    bump(w, 1);
    sd.phase = ENG_PHASE_FIXED_UPDATE;
    sd.name = "counter.tick";
    sd.fn = tick;
    return engCreateSystem(w, &sd, &s);
}

static void onWorldStop(EngWorld* w, void* state) {
    (void)state;
    bump(w, 2);   /* and its system is left for the host to destroy */
}

ENG_MODULE_EXPORT const EngModuleDesc* engModuleEntry(EngGetProcFn get_proc);
ENG_MODULE_EXPORT const EngModuleDesc* engModuleEntry(EngGetProcFn get_proc) {
    static const EngModuleDesc module = {
        sizeof(EngModuleDesc), ENG_API_VERSION, "counter", "0.1", NULL, 0, 0,
        NULL, NULL, onWorldStart, onWorldStop,
    };
    if (engClientLoad(get_proc, NULL) != ENG_OK) return NULL;
    return &module;
}
