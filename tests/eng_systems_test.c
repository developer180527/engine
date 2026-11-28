/* eng_systems_test: systems and blobs through the C API, from C (WO-073).
 *
 * Plain C11 with only <eng/engine.h>. Systems: the run order within a phase
 * (creation order, moved only by run_after, a name not created yet counting
 * once it is), cycles and cross-phase names refused by name, the three phases
 * at their rates and in their order, a system over a query writing in place,
 * systems made and destroyed from inside a system. Blobs: the bytes behind a
 * BLOB field, owned by the field that holds them, copied when a second field
 * stores the same handle, freed when the field lets go, and seen by the
 * change stream when their bytes change. */
#include <eng/engine.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  " __VA_ARGS__); printf("  (line %d: %s)\n", __LINE__, engGetLastErrorMessage()); ++g_failures; } \
                           else { printf("  ok    " __VA_ARGS__); printf("\n"); } } while (0)

/* ── A log of which system ran, as one letter each ── */
static char g_log[256];
static size_t g_logLen = 0;
static void logc(char c) { if (g_logLen + 1 < sizeof g_log) { g_log[g_logLen++] = c; g_log[g_logLen] = 0; } }
static void logClear(void) { g_logLen = 0; g_log[0] = 0; }

static void letter(const EngSystemContext* ctx, const EngQueryBatch* b) {
    (void)b;
    logc(*(const char*)ctx->user);
}

static EngWorld* g_world;

static EngResult makeSystem(const char* name, EngPhase phase, const char* user, const char* const* after, uint32_t n,
                            EngSystem* out) {
    EngSystemDesc d = ENG_INIT(EngSystemDesc);
    d.name = name;
    d.phase = phase;
    d.fn = letter;
    d.user = (void*)user;
    d.run_after = after;
    d.run_after_count = n;
    return engCreateSystem(g_world, &d, out);
}

/* One frame of exactly one simulation step: 1/60 s from a zero accumulator
 * may land a hair short, so step until a step has happened. */
static uint32_t stepOnce(EngEngine* e) {
    EngStepInfo info = ENG_INIT(EngStepInfo);
    engStepWithDelta(e, 1.0 / 60.0, &info);
    return info.simulation_steps;
}

/* ── Phases: what each saw ── */
typedef struct PhaseSeen {
    uint32_t calls;
    double delta;
    uint64_t tick;
} PhaseSeen;
static PhaseSeen g_seen[3];
static void watchPhase(const EngSystemContext* ctx, const EngQueryBatch* b) {
    (void)b;
    PhaseSeen* s = &g_seen[*(const int*)ctx->user];
    ++s->calls;
    s->delta = ctx->delta_seconds;
    s->tick = ctx->tick;
    logc((char)('0' + *(const int*)ctx->user));
}

/* ── A system over a query ── */
typedef struct Speed { float value; uint32_t pad; } Speed;
static uint32_t g_batches = 0, g_rows = 0;
static EngBool g_hadIter = ENG_FALSE;
static void addSpeed(const EngSystemContext* ctx, const EngQueryBatch* b) {
    Speed* s = (Speed*)b->columns[0];
    for (uint32_t i = 0; i < b->count; ++i) s[i].value += 1.0f;
    ++g_batches;
    g_rows += b->count;
    g_hadIter = ctx->iter != NULL;
}

/* ── Systems from inside systems ── */
static EngSystem g_selfDestroying = 0;
static uint32_t g_selfRuns = 0;
static void destroysItself(const EngSystemContext* ctx, const EngQueryBatch* b) {
    (void)b;
    ++g_selfRuns;
    engDestroySystem(ctx->world, g_selfDestroying);
}
static uint32_t g_childRuns = 0;
static void child(const EngSystemContext* ctx, const EngQueryBatch* b) { (void)ctx; (void)b; ++g_childRuns; }
static EngBool g_spawned = ENG_FALSE;
static EngResult g_spawnResult = -1;
static EngBool g_destroyRefused = ENG_FALSE;
static void spawner(const EngSystemContext* ctx, const EngQueryBatch* b) {
    (void)b;
    if (g_spawned) return;
    g_spawned = ENG_TRUE;
    EngSystemDesc d = ENG_INIT(EngSystemDesc);
    d.name = "t.child";
    d.phase = ENG_PHASE_FIXED_UPDATE;
    d.fn = child;
    EngSystem s = 0;
    g_spawnResult = engCreateSystem(ctx->world, &d, &s);
    engDestroyWorld(ctx->world);   /* refused: the handle stays */
    g_destroyRefused = strstr(engGetLastErrorMessage(), "inside a system") != NULL ? ENG_TRUE : ENG_FALSE;
}

/* ── Blobs ── */
typedef struct Path {
    EngBlob points;
    float length;
    uint32_t pad;
} Path;
static const EngFieldDesc kPathFields[] = {
    { "points", ENG_FIELD_BLOB, offsetof(Path, points), 1, 0 },
    { "length", ENG_FIELD_F32,  offsetof(Path, length), 1, 0 },
};

static EngBool blobIs(EngBlob b, const char* text) {
    const void* p = NULL;
    uint32_t n = 0;
    if (engGetBlob(g_world, b, &p, &n) != ENG_OK) return ENG_FALSE;
    return n == strlen(text) && (n == 0 || memcmp(p, text, n) == 0) ? ENG_TRUE : ENG_FALSE;
}
static EngBool blobGone(EngBlob b) {
    const void* p = NULL;
    uint32_t n = 0;
    return engGetBlob(g_world, b, &p, &n) == ENG_ERROR_STALE_HANDLE ? ENG_TRUE : ENG_FALSE;
}
static EngBlob blobOf(EngComponent path, EngEntity e) {
    Path v;
    memset(&v, 0, sizeof v);
    engGetComponent(g_world, e, path, &v, sizeof v);
    return v.points;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("eng_systems_test\n");

    EngEngineDesc ed = ENG_INIT(EngEngineDesc);
    ed.api_version = ENG_API_VERSION;
    EngEngine* engine = NULL;
    CHECK(engCreateEngine(&ed, &engine) == ENG_OK, "an engine");
    CHECK(engCreateWorld(engine, NULL, &g_world) == ENG_OK, "its world");

    /* ── Order within a phase ── */
    static const char A = 'a', B = 'b', C = 'c', D = 'd', Z = 'z';
    const char* afterB[] = { "t.b" };
    const char* afterA[] = { "t.a" };
    const char* afterZ[] = { "t.z" };
    EngSystem sc = 0, sa = 0, sb = 0, sd = 0, sz = 0;
    CHECK(makeSystem("t.c", ENG_PHASE_FIXED_UPDATE, &C, afterB, 1, &sc) == ENG_OK, "t.c, after t.b (not created yet)");
    CHECK(makeSystem("t.a", ENG_PHASE_FIXED_UPDATE, &A, NULL, 0, &sa) == ENG_OK, "t.a");
    CHECK(makeSystem("t.b", ENG_PHASE_FIXED_UPDATE, &B, afterA, 1, &sb) == ENG_OK, "t.b, after t.a");
    CHECK(makeSystem("t.d", ENG_PHASE_FIXED_UPDATE, &D, afterZ, 1, &sd) == ENG_OK, "t.d, after t.z (not created yet)");
    logClear();
    while (!stepOnce(engine)) {}
    CHECK(strcmp(g_log, "abcd") == 0,
          "creation order moved only by run_after: %s (want abcd)", g_log);
    CHECK(makeSystem("t.z", ENG_PHASE_FIXED_UPDATE, &Z, NULL, 0, &sz) == ENG_OK, "t.z created: t.d now waits for it");
    logClear();
    while (!stepOnce(engine)) {}
    CHECK(strcmp(g_log, "abczd") == 0, "a name created later counts once it is: %s (want abczd)", g_log);

    /* ── Refusals ── */
    {
        EngSystem s = 0, sx = 0;
        const char* afterX[] = { "t.x" };
        const char* afterY[] = { "t.y" };
        CHECK(makeSystem("t.x", ENG_PHASE_FIXED_UPDATE, &A, afterY, 1, &sx) == ENG_OK, "t.x, after t.y");
        CHECK(makeSystem("t.y", ENG_PHASE_FIXED_UPDATE, &A, afterX, 1, &s) == ENG_ERROR_REFUSED && !s,
              "t.y after t.x closes a cycle, refused: %s", engGetLastErrorMessage());
        CHECK(strstr(engGetLastErrorMessage(), "t.x -> t.y") || strstr(engGetLastErrorMessage(), "t.y -> t.x"),
              "the message names the cycle");
        CHECK(makeSystem("t.y", ENG_PHASE_UPDATE, &A, NULL, 0, &s) == ENG_ERROR_REFUSED,
              "t.y in another phase than t.x, which runs after it, refused: %s", engGetLastErrorMessage());
        CHECK(makeSystem("t.late", ENG_PHASE_LATE_UPDATE, &A, afterA, 1, &s) == ENG_ERROR_REFUSED,
              "run_after a system of another phase, refused: %s", engGetLastErrorMessage());
        CHECK(makeSystem("t.a", ENG_PHASE_FIXED_UPDATE, &A, NULL, 0, &s) == ENG_ERROR_REFUSED, "a name taken, refused");
        CHECK(makeSystem("plain", ENG_PHASE_FIXED_UPDATE, &A, NULL, 0, &s) == ENG_ERROR_INVALID_ARGUMENT, "a name not namespaced, refused");
        const char* self[] = { "t.self" };
        CHECK(makeSystem("t.self", ENG_PHASE_FIXED_UPDATE, &A, self, 1, &s) == ENG_ERROR_INVALID_ARGUMENT, "after itself, refused");
        EngSystemDesc d = ENG_INIT(EngSystemDesc);
        d.name = "t.parallel";
        d.fn = letter;
        d.parallel = ENG_TRUE;
        CHECK(engCreateSystem(g_world, &d, &s) == ENG_ERROR_UNSUPPORTED, "parallel refused by name: %s", engGetLastErrorMessage());
        d.parallel = ENG_FALSE;
        d.phase = 7;
        CHECK(engCreateSystem(g_world, &d, &s) == ENG_ERROR_INVALID_ARGUMENT, "an unknown phase, refused");
        engDestroySystem(g_world, sa);
        engDestroySystem(g_world, sb);
        engDestroySystem(g_world, sc);
        engDestroySystem(g_world, sd);
        engDestroySystem(g_world, sz);
        CHECK(engCreateSystem(g_world, NULL, &s) == ENG_ERROR_INVALID_ARGUMENT, "no desc, refused");
        logClear();
        while (!stepOnce(engine)) {}
        CHECK(strcmp(g_log, "a") == 0, "destroyed systems do not run (t.x is left): %s", g_log);
        CHECK(makeSystem("t.y", ENG_PHASE_UPDATE, &A, NULL, 0, &s) == ENG_ERROR_REFUSED, "t.x still names t.y in FIXED_UPDATE");
        engDestroySystem(g_world, sa);   /* stale: ignored with an error */
        CHECK(strstr(engGetLastErrorMessage(), "not a live system") != NULL, "a stale system handle, refused");
        engDestroySystem(g_world, sx);
        CHECK(makeSystem("t.y", ENG_PHASE_UPDATE, &A, NULL, 0, &s) == ENG_OK, "with t.x gone, t.y may be in UPDATE");
        engDestroySystem(g_world, s);
    }

    /* ── Phases, rates and order ── */
    static const int P0 = 0, P1 = 1, P2 = 2;
    EngSystem ph[3];
    for (int i = 0; i < 3; ++i) {
        EngSystemDesc d = ENG_INIT(EngSystemDesc);
        static const char* names[] = { "t.fixed", "t.update", "t.late" };
        static const int* users[] = { &P0, &P1, &P2 };
        d.name = names[i];
        d.phase = (EngPhase)i;
        d.fn = watchPhase;
        d.user = (void*)users[i];
        CHECK(engCreateSystem(g_world, &d, &ph[i]) == ENG_OK, "a system in phase %d", i);
    }
    memset(g_seen, 0, sizeof g_seen);
    uint32_t steps = 0;
    EngStepInfo info = ENG_INIT(EngStepInfo);
    for (int f = 0; f < 10; ++f) {
        info = (EngStepInfo)ENG_INIT(EngStepInfo);
        engStepWithDelta(engine, 1.0 / 30.0, &info);
        steps += info.simulation_steps;
    }
    CHECK(g_seen[0].calls == steps && steps >= 18, "FIXED_UPDATE once per simulation step (%u of %u)", g_seen[0].calls, steps);
    CHECK(g_seen[1].calls == 10 && g_seen[2].calls == 10, "UPDATE and LATE_UPDATE once per frame (%u, %u)", g_seen[1].calls, g_seen[2].calls);
    CHECK(g_seen[0].delta == 1.0 / 60.0, "FIXED_UPDATE's delta is the step, 1/60 exactly");
    CHECK(g_seen[1].delta > 0.0333 && g_seen[1].delta < 0.0334, "UPDATE's delta is the frame's (%f)", g_seen[1].delta);
    CHECK(g_seen[0].tick == g_seen[1].tick && g_seen[0].tick > 0, "the tick counts steps (%llu)", (unsigned long long)g_seen[0].tick);
    logClear();
    info = (EngStepInfo)ENG_INIT(EngStepInfo);
    engStepWithDelta(engine, 1.0 / 30.0, &info);
    {
        char want[16] = { 0 };
        uint32_t k = 0;
        for (; k < info.simulation_steps && k < 8; ++k) want[k] = '0';
        want[k] = '1'; want[k + 1] = '2';
        CHECK(info.simulation_steps >= 1 && strcmp(g_log, want) == 0,
              "the frame's steps' FIXED_UPDATE, then UPDATE, then LATE_UPDATE: %s (want %s)", g_log, want);
    }
    for (int i = 0; i < 3; ++i) engDestroySystem(g_world, ph[i]);

    /* ── A system over a query, writing in place ── */
    {
        const EngFieldDesc sf[] = { { "value", ENG_FIELD_F32, 0, 1, 0 } };
        EngComponentDesc cd = ENG_INIT(EngComponentDesc);
        cd.name = "t.Speed"; cd.size = sizeof(Speed); cd.align = _Alignof(Speed); cd.fields = sf; cd.field_count = 1;
        EngComponent speed = 0;
        CHECK(engRegisterComponent(g_world, &cd, &speed) == ENG_OK, "t.Speed");
        EngEntity es[5];
        for (int i = 0; i < 5; ++i) { engCreateEntity(g_world, NULL, &es[i]); engAddComponent(g_world, es[i], speed, NULL); }
        EngQueryTerm t = { speed, ENG_ACCESS_WRITE, { 0, 0 } };
        EngQueryDesc qd = ENG_INIT(EngQueryDesc);
        qd.term_count = 1; qd.terms = &t;
        EngQuery q = 0;
        engCreateQuery(g_world, &qd, &q);
        EngSystemDesc d = ENG_INIT(EngSystemDesc);
        d.name = "t.add_speed"; d.phase = ENG_PHASE_FIXED_UPDATE; d.query = q; d.fn = addSpeed;
        EngSystem s = 0;
        CHECK(engCreateSystem(g_world, &d, &s) == ENG_OK, "a system over a query");
        uint32_t n = 0;
        while (!(n = stepOnce(engine))) {}
        Speed v = { 0, 0 };
        engGetComponent(g_world, es[3], speed, &v, sizeof v);
        CHECK(g_rows == 5 * n && g_batches >= 1 && v.value == (float)n && g_hadIter,
              "called per batch, writing the column in place (%u rows, value %.0f)", g_rows, v.value);
        engDestroyQuery(g_world, q);
        CHECK(strstr(engGetLastErrorMessage(), "t.add_speed") != NULL, "its query cannot be destroyed while it runs it: %s",
              engGetLastErrorMessage());
        engDestroySystem(g_world, s);
        engDestroyQuery(g_world, q);
        EngQueryIter* it = NULL;
        CHECK(engQueryBegin(g_world, q, &it) == ENG_ERROR_STALE_HANDLE, "then it can");
    }

    /* ── Systems made and destroyed from inside a system ── */
    {
        EngSystemDesc d = ENG_INIT(EngSystemDesc);
        d.name = "t.once"; d.phase = ENG_PHASE_FIXED_UPDATE; d.fn = destroysItself;
        CHECK(engCreateSystem(g_world, &d, &g_selfDestroying) == ENG_OK, "a system that destroys itself");
        d.name = "t.spawner"; d.fn = spawner;
        EngSystem sp = 0;
        CHECK(engCreateSystem(g_world, &d, &sp) == ENG_OK, "a system that creates one");
        for (int i = 0; i < 4; ++i) while (!stepOnce(engine)) {}
        CHECK(g_selfRuns == 1, "destroyed from inside itself, it does not run again (%u)", g_selfRuns);
        CHECK(g_spawnResult == ENG_OK && g_childRuns == 3, "created from inside a system, it runs from the next step (%u)", g_childRuns);
        EngEntity probe = 0;
        CHECK(g_destroyRefused && engCreateEntity(g_world, NULL, &probe) == ENG_OK,
              "engDestroyWorld from inside a system is refused, and the handle still works");
        engDestroySystem(g_world, sp);
    }

    /* ── Blobs ── */
    {
        EngComponentDesc cd = ENG_INIT(EngComponentDesc);
        cd.name = "t.Path"; cd.size = sizeof(Path); cd.align = _Alignof(Path); cd.fields = kPathFields; cd.field_count = 2;
        EngComponent path = 0;
        Path bad = { 77, 0, 0 };
        cd.defaults = &bad;
        CHECK(engRegisterComponent(g_world, &cd, &path) == ENG_ERROR_INVALID_ARGUMENT, "a BLOB default that is not 0, refused");
        cd.defaults = NULL;
        EngFieldDesc two[2];
        memcpy(two, kPathFields, sizeof two);
        two[0].count = 2;
        cd.fields = two;
        cd.name = "t.Path2";
        CHECK(engRegisterComponent(g_world, &cd, &path) == ENG_ERROR_INVALID_ARGUMENT, "a BLOB array, refused");
        cd.name = "t.Path"; cd.fields = kPathFields;
        CHECK(engRegisterComponent(g_world, &cd, &path) == ENG_OK, "a component with a BLOB field");

        EngBlob b1 = 0, b2 = 0, empty = 0;
        CHECK(engCreateBlob(g_world, "one-two", 7, &b1) == ENG_OK && b1 && blobIs(b1, "one-two"), "a blob, made from bytes");
        CHECK(engCreateBlob(g_world, NULL, 0, &empty) == ENG_OK && blobIs(empty, ""), "an empty one");
        CHECK(engCreateBlob(g_world, NULL, (16u << 20) + 1, &b2) == ENG_ERROR_INVALID_ARGUMENT, "over 16 MiB, refused");

        EngEntity e1 = 0, e2 = 0;
        engCreateEntity(g_world, NULL, &e1);
        engCreateEntity(g_world, NULL, &e2);
        Path v = { b1, 2.5f, 0 };
        CHECK(engAddComponent(g_world, e1, path, &v) == ENG_OK && blobOf(path, e1) == b1, "stored in a field: the field holds it");
        CHECK(engAddComponent(g_world, e2, path, &v) == ENG_OK, "the same handle stored in a second field");
        const EngBlob copy = blobOf(path, e2);
        CHECK(copy && copy != b1 && blobIs(copy, "one-two") && blobIs(b1, "one-two"),
              "the second field holds a copy, never the same bytes");

        CHECK(engCreateBlob(g_world, "three", 5, &b2) == ENG_OK, "another blob");
        v.points = b2;
        engSetComponent(g_world, e1, path, &v, sizeof v);
        CHECK(blobGone(b1) && blobIs(b2, "three"), "the field set to another: the one it held is freed");
        v.points = 0;
        engSetComponent(g_world, e1, path, &v, sizeof v);
        CHECK(blobGone(b2), "set to 0: freed");
        EngBlob b3 = 0;
        engCreateBlob(g_world, "x", 1, &b3);
        CHECK(blobIs(b3, "x"), "a blob nobody holds stays until the world ends");

        /* The change stream sees a blob's bytes change through its holder. */
        EngChangeStream cs = 0;
        CHECK(engSubscribeChanges(g_world, path, &cs) == ENG_OK, "a stream on t.Path");
        EngChange ch[8];
        uint32_t n = 8;
        engReadChanges(g_world, cs, ch, &n);   /* the seed: both as ADDED */
        CHECK(engSetBlob(g_world, copy, "four", 4) == ENG_OK && blobIs(copy, "four"), "engSetBlob replaces the bytes, same handle");
        n = 8;
        engReadChanges(g_world, cs, ch, &n);
        CHECK(n == 1 && ch[0].entity == e2 && ch[0].kind == ENG_CHANGE_CHANGED, "and the holder's component counts as changed (%u)", n);
        engUnsubscribeChanges(g_world, cs);

        /* A query's WRITE column: handles written in place are settled at the batch's end. */
        EngBlob b4 = 0;
        engCreateBlob(g_world, "five", 4, &b4);
        EngQueryTerm t = { path, ENG_ACCESS_WRITE, { 0, 0 } };
        EngQueryDesc qd = ENG_INIT(EngQueryDesc);
        qd.term_count = 1; qd.terms = &t;
        EngQuery q = 0;
        engCreateQuery(g_world, &qd, &q);
        EngQueryIter* it = NULL;
        engQueryBegin(g_world, q, &it);
        EngQueryBatch batch = ENG_INIT(EngQueryBatch);
        while (engQueryNext(it, &batch)) {
            Path* col = (Path*)batch.columns[0];
            for (uint32_t i = 0; i < batch.count; ++i) {
                if (batch.entities[i] == e1) col[i].points = b4;     /* nobody holds it: taken */
                if (batch.entities[i] == e2) col[i].points = b4;     /* now e1 holds it: a copy */
            }
        }
        engQueryEnd(it);
        const EngBlob e2now = blobOf(path, e2);
        CHECK(blobOf(path, e1) == b4 && e2now != b4 && blobIs(e2now, "five") && blobGone(copy),
              "a WRITE column: taken, copied, and the replaced one freed");
        engDestroyQuery(g_world, q);

        CHECK(engRemoveComponent(g_world, e1, path) == ENG_OK && blobGone(b4), "the component removed: its blob freed");
        engDestroyEntity(g_world, e2);
        CHECK(blobGone(e2now), "the entity destroyed: its blob freed");
        CHECK(engSetBlob(g_world, b4, "z", 1) == ENG_ERROR_STALE_HANDLE, "a freed handle, refused");
        EngBlob b5 = 0;
        engCreateBlob(g_world, "six", 3, &b5);
        CHECK(b5 != b4 && b5 != b1 && b5 != b2 && b5 != copy && b5 != e2now && blobGone(e2now),
              "a new blob never reuses a handle, not even the slot just freed");
        while (!stepOnce(engine)) {}   /* the sim hash reads blob fields */
    }

    engDestroyWorld(g_world);
    engDestroyEngine(engine);
    printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
