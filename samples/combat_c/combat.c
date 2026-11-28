/* combat.c: the combat kit, on the engine's C API (WO-073). See combat.h.
 * Built with only include/ on its include path: a flecs or engine C++ include
 * here fails to compile, which is the point. */
#include "combat.h"

#include <stddef.h>
#include <stdlib.h>

typedef struct CombatState {
    EngComponent health, inbox, died;
    EngQuery inboxes;
    EngSystem apply;
    float damage_multiplier;   /* 1: damage as dealt */
    EngBool leave_corpse;      /* true: tag Died; false: destroy at once */
} CombatState;

/* Once per simulation step: drain every inbox into Health. Removing,
 * setting and destroying here are queued, and applied when the step's
 * iteration ends. */
static void applyDamage(const EngSystemContext* ctx, const EngQueryBatch* batch) {
    const CombatState* s = (const CombatState*)ctx->user;
    EngWorld* w = ctx->world;
    const CombatDamageInbox* in = (const CombatDamageInbox*)batch->columns[0];
    const float now = (float)((double)ctx->tick * ctx->delta_seconds);
    uint32_t i;
    for (i = 0; i < batch->count; ++i) {
        const EngEntity e = batch->entities[i];
        CombatHealth h;
        const float dmg = in[i].pending;
        /* Drained whatever happens next, so a stray hit never lingers. */
        engRemoveComponent(w, e, s->inbox);
        if (!(dmg > 0.0f) || !engHasComponent(w, e, s->health)) continue;
        if (engGetComponent(w, e, s->health, &h, (uint32_t)sizeof h) != ENG_OK) continue;

        /* The rule, v1: flat subtraction times a multiplier. Armor,
         * resistances, damage types and crits all go here. */
        h.current -= dmg * s->damage_multiplier;
        if (h.current > 0.0f) { engSetComponent(w, e, s->health, &h, (uint32_t)sizeof h); continue; }

        h.current = 0.0f;
        engSetComponent(w, e, s->health, &h, (uint32_t)sizeof h);
        if (engHasComponent(w, e, s->died)) continue;   /* died already */
        if (s->leave_corpse) {
            const CombatDied d = { now };
            engAddComponent(w, e, s->died, &d);
        } else {
            engDestroyEntity(w, e);
        }
    }
}

static EngResult registerOne(EngWorld* w, const char* name, uint32_t size, const EngFieldDesc* fields,
                             uint32_t count, const void* defaults, EngComponent* out) {
    EngComponentDesc d = ENG_INIT(EngComponentDesc);
    d.name = name;
    d.size = size;
    d.align = 4;
    d.fields = fields;
    d.field_count = count;
    d.defaults = defaults;
    return engRegisterComponent(w, &d, out);
}

static EngResult onLoad(EngEngine* engine, void** state) {
    CombatState* s = (CombatState*)calloc(1, sizeof *s);
    (void)engine;
    if (!s) return ENG_ERROR_OUT_OF_MEMORY;
    s->damage_multiplier = 1.0f;
    s->leave_corpse = ENG_TRUE;
    *state = s;
    return ENG_OK;
}

static void onUnload(EngEngine* engine, void* state) {
    (void)engine;
    free(state);
}

static EngResult onWorldStart(EngWorld* w, void* state) {
    static const EngFieldDesc healthFields[] = {
        { "current", ENG_FIELD_F32, offsetof(CombatHealth, current), 1, 0 },
        { "max", ENG_FIELD_F32, offsetof(CombatHealth, max), 1, 0 },
    };
    static const EngFieldDesc inboxFields[] = {
        { "pending", ENG_FIELD_F32, offsetof(CombatDamageInbox, pending), 1, 0 },
    };
    static const EngFieldDesc diedFields[] = {
        { "at_sim_time", ENG_FIELD_F32, offsetof(CombatDied, at_sim_time), 1, 0 },
    };
    static const CombatHealth fullHealth = { 100.0f, 100.0f };
    CombatState* s = (CombatState*)state;
    EngQueryTerm term;
    EngQueryDesc qd = ENG_INIT(EngQueryDesc);
    EngSystemDesc sd = ENG_INIT(EngSystemDesc);
    EngResult r;

    if ((r = registerOne(w, COMBAT_HEALTH, sizeof(CombatHealth), healthFields, 2, &fullHealth, &s->health)) != ENG_OK ||
        (r = registerOne(w, COMBAT_DAMAGE_INBOX, sizeof(CombatDamageInbox), inboxFields, 1, NULL, &s->inbox)) != ENG_OK ||
        (r = registerOne(w, COMBAT_DIED, sizeof(CombatDied), diedFields, 1, NULL, &s->died)) != ENG_OK)
        return r;

    term.component = s->inbox;
    term.access = ENG_ACCESS_READ;
    term.reserved[0] = 0;
    term.reserved[1] = 0;
    qd.terms = &term;
    qd.term_count = 1;
    if ((r = engCreateQuery(w, &qd, &s->inboxes)) != ENG_OK) return r;

    sd.phase = ENG_PHASE_FIXED_UPDATE;
    sd.name = "combat.apply_damage";
    sd.query = s->inboxes;
    sd.fn = applyDamage;
    sd.user = s;
    return engCreateSystem(w, &sd, &s->apply);
}

static void onWorldStop(EngWorld* w, void* state) {
    CombatState* s = (CombatState*)state;
    engDestroySystem(w, s->apply);
    engDestroyQuery(w, s->inboxes);
    s->apply = 0;
    s->inboxes = 0;
}

const EngModuleDesc* combatKit(void) {
    static const EngModuleDesc module = {
        sizeof(EngModuleDesc), ENG_API_VERSION, "combat", "1.0.0",
        NULL, 0, 0,
        onLoad, onUnload, onWorldStart, onWorldStop,
    };
    return &module;
}
