/* eng_module_test: modules linked into a program, and the combat kit on them,
 * from C (WO-073 part 3).
 *
 * Plain C11 with only <eng/engine.h> and the kit's own header. Modules: the
 * four callbacks at their moments and in their order (load and world start in
 * list order, world stop and unload in reverse), each given its own state, a
 * failing on_load or on_world_start undoing the ones before it and naming the
 * module, worlds not created or destroyed from inside a callback, and a bad
 * module list refused by name. The kit (samples/combat_c): damage drained
 * into Health once per step, only entities with Health hurt, Died added once,
 * and a second module dealing damage from its own system. */
#include <eng/engine.h>

#include "combat.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  " __VA_ARGS__); printf("  (line %d: %s)\n", __LINE__, engGetLastErrorMessage()); ++g_failures; } \
                           else { printf("  ok    " __VA_ARGS__); printf("\n"); } } while (0)
#define HAS(text) (strstr(engGetLastErrorMessage(), (text)) != NULL)

static uint32_t stepOnce(EngEngine* e) {
    EngStepInfo info = ENG_INIT(EngStepInfo);
    engStepWithDelta(e, 1.0 / 60.0, &info);
    return info.simulation_steps;
}
static void steps(EngEngine* e, int n) {
    for (int i = 0; i < n; ++i) while (!stepOnce(e)) {}
}

/* ── Two modules that log what happened to them, one letter each:
 *    a: L S T U (load, world start, world stop, unload); b: the same, lower. ── */
static char g_log[64];
static void logc(char c) { size_t n = strlen(g_log); if (n + 1 < sizeof g_log) { g_log[n] = c; g_log[n + 1] = 0; } }
static void logClear(void) { g_log[0] = 0; }

static int g_tokenA = 1, g_tokenB = 2;
static EngBool g_stateOk = ENG_TRUE;
static EngBool g_failLoadB = ENG_FALSE, g_failStartB = ENG_FALSE, g_tryWorldCalls = ENG_FALSE;
static EngResult g_createInside = ENG_OK;
static EngBool g_destroyRefused = ENG_FALSE;
static EngWorld* g_startedWith = NULL;

static EngResult loadA(EngEngine* e, void** state) { (void)e; *state = &g_tokenA; logc('L'); return ENG_OK; }
static EngResult loadB(EngEngine* e, void** state) {
    (void)e;
    *state = &g_tokenB;
    logc('l');
    return g_failLoadB ? ENG_ERROR_REFUSED : ENG_OK;
}
static void unloadA(EngEngine* e, void* state) { (void)e; if (state != &g_tokenA) g_stateOk = ENG_FALSE; logc('U'); }
static void unloadB(EngEngine* e, void* state) { (void)e; if (state != &g_tokenB) g_stateOk = ENG_FALSE; logc('u'); }
static EngResult startA(EngWorld* w, void* state) {
    if (state != &g_tokenA) g_stateOk = ENG_FALSE;
    g_startedWith = w;
    logc('S');
    if (g_tryWorldCalls) {
        engDestroyWorld(w);
        g_destroyRefused = HAS("module's callback") ? ENG_TRUE : ENG_FALSE;
    }
    return ENG_OK;
}
static EngEngine* g_engineForInside = NULL;
static EngResult startB(EngWorld* w, void* state) {
    (void)w;
    if (state != &g_tokenB) g_stateOk = ENG_FALSE;
    logc('s');
    if (g_tryWorldCalls && g_engineForInside) {
        EngWorld* second = NULL;
        g_createInside = engCreateWorld(g_engineForInside, NULL, &second);
    }
    if (g_failStartB) {
        engFindComponent(w, "b.nothing");   /* leaves a message the failure carries */
        return ENG_ERROR_NOT_FOUND;
    }
    return ENG_OK;
}
static void stopA(EngWorld* w, void* state) { (void)w; if (state != &g_tokenA) g_stateOk = ENG_FALSE; logc('T'); }
static void stopB(EngWorld* w, void* state) { (void)w; if (state != &g_tokenB) g_stateOk = ENG_FALSE; logc('t'); }

static const EngModuleDesc g_modA = { sizeof(EngModuleDesc), ENG_API_VERSION, "a", "1.0", NULL, 0, 0,
                                      loadA, unloadA, startA, stopA };
static const EngModuleDesc g_modB = { sizeof(EngModuleDesc), ENG_API_VERSION, "b", NULL, NULL, 0, 0,
                                      loadB, unloadB, startB, stopB };
static const EngModuleDesc g_bare = { sizeof(EngModuleDesc), ENG_API_VERSION, "bare", NULL, NULL, 0, 0,
                                      NULL, NULL, NULL, NULL };

static EngEngineDesc engineDesc(const EngModuleDesc* const* mods, uint32_t n) {
    EngEngineDesc d = ENG_INIT(EngEngineDesc);
    d.api_version = ENG_API_VERSION;
    d.modules = mods;
    d.module_count = n;
    return d;
}

/* ── A turret: a second module whose system deals damage through combat.h ── */
static EngEntity g_turretTarget = 0;
static int g_turretShots = 0;
static void turretFire(const EngSystemContext* ctx, const EngQueryBatch* b) {
    (void)b;
    if (g_turretShots <= 0 || !g_turretTarget) return;
    if (combatDealDamage(ctx->world, g_turretTarget, 10.0f) == ENG_OK) --g_turretShots;
}
static EngResult turretStart(EngWorld* w, void* state) {
    EngSystemDesc sd = ENG_INIT(EngSystemDesc);
    EngSystem s = 0;
    (void)state;
    sd.phase = ENG_PHASE_FIXED_UPDATE;
    sd.name = "turret.fire";
    sd.fn = turretFire;
    return engCreateSystem(w, &sd, &s);
}
static const EngModuleDesc g_turret = { sizeof(EngModuleDesc), ENG_API_VERSION, "turret", NULL, NULL, 0, 0,
                                        NULL, NULL, turretStart, NULL };

static float healthOf(EngWorld* w, EngComponent health, EngEntity e) {
    CombatHealth h = { -1.0f, -1.0f };
    engGetComponent(w, e, health, &h, (uint32_t)sizeof h);
    return h.current;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("eng_module_test\n");
    EngEngine* engine = NULL;
    EngWorld* world = NULL;

    /* ── The four moments, in order ── */
    {
        const EngModuleDesc* mods[] = { &g_modA, &g_modB, &g_bare };
        EngEngineDesc d = engineDesc(mods, 3);
        logClear();
        CHECK(engCreateEngine(&d, &engine) == ENG_OK, "an engine with three modules");
        CHECK(strcmp(g_log, "Ll") == 0, "on_load in list order (log '%s'); a module with no callbacks is fine", g_log);
        CHECK(engCreateWorld(engine, NULL, &world) == ENG_OK, "its world");
        CHECK(strcmp(g_log, "LlSs") == 0, "on_world_start in list order (log '%s')", g_log);
        CHECK(g_startedWith == world, "on_world_start is given the world engCreateWorld returns");
        CHECK(combatDealDamage(world, 0, 1.0f) == ENG_ERROR_NOT_FOUND, "combat.h without the combat kit: not found");
        engDestroyWorld(world);
        CHECK(strcmp(g_log, "LlSstT") == 0, "on_world_stop in reverse (log '%s')", g_log);
        CHECK(engCreateWorld(engine, NULL, &world) == ENG_OK, "a world again");
        CHECK(strcmp(g_log, "LlSstTSs") == 0, "on_world_start again for it (log '%s')", g_log);

        /* No world created or destroyed under a callback. */
        engDestroyWorld(world);
        g_tryWorldCalls = ENG_TRUE;
        g_engineForInside = engine;
        CHECK(engCreateWorld(engine, NULL, &world) == ENG_OK, "a world whose modules try to create and destroy worlds");
        CHECK(g_destroyRefused, "engDestroyWorld from on_world_start, refused by name");
        CHECK(g_createInside == ENG_ERROR_REFUSED, "engCreateWorld from on_world_start, refused");
        g_tryWorldCalls = ENG_FALSE;
        EngEntity probe = 0;
        CHECK(engCreateEntity(world, NULL, &probe) == ENG_OK, "and the world still works");

        logClear();
        engDestroyEngine(engine);
        CHECK(strcmp(g_log, "tTuU") == 0, "engDestroyEngine with a world held: stop, then unload, in reverse (log '%s')", g_log);
        CHECK(g_stateOk, "every callback was given its own module's state");
        engine = NULL;
        world = NULL;
    }

    /* ── Failures undo what came before, and name the module ── */
    {
        const EngModuleDesc* mods[] = { &g_modA, &g_modB };
        EngEngineDesc d = engineDesc(mods, 2);
        g_failLoadB = ENG_TRUE;
        logClear();
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_REFUSED && engine == NULL, "b's on_load fails: so does the create, with its result");
        CHECK(HAS("module 'b'") && HAS("on_load"), "naming the module and the callback");
        CHECK(strcmp(g_log, "LlU") == 0, "and a, loaded before it, is unloaded (log '%s')", g_log);
        g_failLoadB = ENG_FALSE;

        CHECK(engCreateEngine(&d, &engine) == ENG_OK, "an engine again");
        g_failStartB = ENG_TRUE;
        logClear();
        CHECK(engCreateWorld(engine, NULL, &world) == ENG_ERROR_NOT_FOUND && world == NULL,
              "b's on_world_start fails: so does engCreateWorld");
        CHECK(HAS("module 'b'") && HAS("on_world_start") && HAS("b.nothing"),
              "naming the module, with the message it left");
        CHECK(strcmp(g_log, "SsT") == 0, "and a, started before it, is stopped (log '%s')", g_log);
        g_failStartB = ENG_FALSE;
        CHECK(engCreateWorld(engine, NULL, &world) == ENG_OK, "the world can be created after that");
        engDestroyWorld(world);
        engDestroyEngine(engine);
        engine = NULL;
        world = NULL;
    }

    /* ── A bad module list, refused by name ── */
    {
        const EngModuleDesc* twice[] = { &g_modA, &g_modA };
        EngEngineDesc d = engineDesc(twice, 2);
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_INVALID_ARGUMENT && HAS("two modules are named 'a'"), "a name twice");
        const EngModuleDesc* holes[] = { &g_modA, NULL };
        d = engineDesc(holes, 2);
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_INVALID_ARGUMENT && HAS("modules[1]"), "a NULL entry, by index");
        d = engineDesc(NULL, 1);
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_INVALID_ARGUMENT && HAS("modules is NULL"), "a count with no array");

        EngModuleDesc m = g_bare;
        const EngModuleDesc* one[] = { &m };
        d = engineDesc(one, 1);
        m.api_version = 0;
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_ABI_MISMATCH && HAS("module 'bare'"), "a module with no api_version");
        m = g_bare;
        static const char* const needs[] = { "engine.physics.v1" };
        m.requires_services = needs;
        m.requires_service_count = 1;
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_UNSUPPORTED && HAS("services"), "a module that needs services");
        m = g_bare;
        m.reserved = 1;
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_INVALID_ARGUMENT && HAS("reserved"), "reserved not zero");
        m = g_bare;
        m.name = "";
        CHECK(engCreateEngine(&d, &engine) == ENG_ERROR_INVALID_ARGUMENT && HAS("no name"), "a module with no name");
        CHECK(engine == NULL, "and no engine from any of them");
    }

    /* ── The combat kit ── */
    {
        const EngModuleDesc* mods[] = { combatKit(), &g_turret };
        EngEngineDesc d = engineDesc(mods, 2);
        CHECK(engCreateEngine(&d, &engine) == ENG_OK, "an engine with the combat kit and a turret");
        CHECK(engCreateWorld(engine, NULL, &world) == ENG_OK, "its world");
        const EngComponent health = engFindComponent(world, COMBAT_HEALTH);
        const EngComponent inbox = engFindComponent(world, COMBAT_DAMAGE_INBOX);
        const EngComponent died = engFindComponent(world, COMBAT_DIED);
        CHECK(health && inbox && died, "the kit registered its three components");

        EngEntity target = 0, wall = 0;
        engCreateEntity(world, "target", &target);
        engCreateEntity(world, "wall", &wall);
        CHECK(engAddComponent(world, target, health, NULL) == ENG_OK && healthOf(world, health, target) == 100.0f,
              "Health starts at its default, 100");

        CHECK(combatDealDamage(world, target, 30.0f) == ENG_OK && engHasComponent(world, target, inbox), "30 damage, in the inbox");
        steps(engine, 1);
        CHECK(healthOf(world, health, target) == 70.0f && !engHasComponent(world, target, inbox),
              "one step: 100 - 30 = 70, and the inbox drained");

        combatDealDamage(world, target, 20.0f);
        combatDealDamage(world, target, 30.0f);
        steps(engine, 1);
        CHECK(healthOf(world, health, target) == 20.0f, "two hits in one step add up: 70 - 50 = 20");

        combatDealDamage(world, wall, 10.0f);
        CHECK(combatDealDamage(world, target, 0.0f) == ENG_OK && combatDealDamage(world, target, -5.0f) == ENG_OK &&
              !engHasComponent(world, target, inbox), "zero or negative damage does nothing");
        steps(engine, 1);
        CHECK(!engHasComponent(world, wall, inbox) && !engHasComponent(world, wall, health),
              "damage to an entity with no Health is dropped, and gives it none");

        combatDealDamage(world, target, 25.0f);
        steps(engine, 1);
        CombatDied first = { -1.0f };
        CHECK(healthOf(world, health, target) == 0.0f && engHasComponent(world, target, died) &&
              engGetComponent(world, target, died, &first, (uint32_t)sizeof first) == ENG_OK && first.at_sim_time > 0.0f,
              "past zero: Health clamps to 0 and Died is added, with the time");
        combatDealDamage(world, target, 5.0f);
        steps(engine, 1);
        CombatDied again = { -1.0f };
        engGetComponent(world, target, died, &again, (uint32_t)sizeof again);
        CHECK(engIsEntityAlive(world, target) && again.at_sim_time == first.at_sim_time && healthOf(world, health, target) == 0.0f,
              "hit again: still 0, Died once, and the entity left for the game to remove");

        /* The turret's own system deals 10 a step, three times. */
        EngEntity dummy = 0;
        engCreateEntity(world, "dummy", &dummy);
        engAddComponent(world, dummy, health, NULL);
        g_turretTarget = dummy;
        g_turretShots = 3;
        steps(engine, 6);
        CHECK(g_turretShots == 0 && healthOf(world, health, dummy) == 70.0f,
              "a second module's system deals damage through combat.h: 3 x 10 off 100");

        engDestroyWorld(world);
        engDestroyEngine(engine);
    }

    printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
