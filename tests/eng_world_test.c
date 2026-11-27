/* eng_world_test: the world through the C API, from C (WO-073).
 *
 * Plain C11 with only <eng/engine.h>: no C++, no flecs, no engine header. A
 * kit's component registered by schema; entities created, parented, found and
 * destroyed; a query's columns read and written in place; the change stream
 * seeing writes by WRITE and only the marked rows by WRITE_MARKED; structural
 * changes during a query applied at its end; stale handles refused. */
#include <eng/engine.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  " __VA_ARGS__); printf("  (line %d: %s)\n", __LINE__, engGetLastErrorMessage()); ++g_failures; } \
                           else { printf("  ok    " __VA_ARGS__); printf("\n"); } } while (0)

typedef struct Health {
    float current, maximum;
    uint32_t team;
    uint32_t pad;      /* explicit: no implicit padding in a component */
    EngEntity target;
} Health;

static const EngFieldDesc kHealthFields[] = {
    { "current", ENG_FIELD_F32,    offsetof(Health, current), 1, 0 },
    { "maximum", ENG_FIELD_F32,    offsetof(Health, maximum), 1, 0 },
    { "team",    ENG_FIELD_U32,    offsetof(Health, team),    1, 0 },
    { "target",  ENG_FIELD_ENTITY, offsetof(Health, target),  1, 0 },
};
static const Health kHealthDefaults = { 100.0f, 100.0f, 0, 0, 0 };

static EngComponentDesc healthDesc(void) {
    EngComponentDesc d = ENG_INIT(EngComponentDesc);
    d.name = "test.Health";
    d.size = sizeof(Health);
    d.align = _Alignof(Health);
    d.fields = kHealthFields;
    d.field_count = 4;
    d.defaults = &kHealthDefaults;
    return d;
}

/* Every change waiting in a stream, in small batches on purpose. */
static uint32_t drain(EngWorld* w, EngChangeStream s, EngChangeKind kind, uint32_t* ofKind) {
    EngChange buf[64];
    uint32_t total = 0, n;
    *ofKind = 0;
    do {
        n = 64;
        if (engReadChanges(w, s, buf, &n) != ENG_OK) return 0xFFFFFFFFu;
        for (uint32_t i = 0; i < n; ++i) if (buf[i].kind == kind) ++*ofKind;
        total += n;
    } while (n == 64);
    return total;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("eng_world_test (C)\n");

    EngEngineDesc ed = ENG_INIT(EngEngineDesc);
    ed.api_version = ENG_API_VERSION;
    ed.app_name = "eng_world_test";
    EngEngine* engine = NULL;
    CHECK(engCreateEngine(&ed, &engine) == ENG_OK, "an engine");

    /* ── The world ── */
    EngWorldDesc wd = ENG_INIT(EngWorldDesc);
    wd.fixed_step_seconds = 1.0 / 30.0;
    EngWorld* world = NULL;
    CHECK(engCreateWorld(engine, &wd, &world) == ENG_ERROR_UNSUPPORTED && !world,
          "a step this version does not run is refused by name: %s", engGetLastErrorMessage());
    CHECK(engCreateWorld(engine, NULL, &world) == ENG_OK && world, "the world");
    EngWorld* second = NULL;
    CHECK(engCreateWorld(engine, NULL, &second) == ENG_ERROR_UNSUPPORTED, "a second world is refused while one is held");

    /* ── Components ── */
    const EngComponent transform = engFindComponent(world, "core.Transform");
    CHECK(transform != 0, "core.Transform is found");
    CHECK(engFindComponent(world, "core.Light") == 0 && strstr(engGetLastErrorMessage(), "core.Transform"),
          "an engine component not exposed yet: 0, saying what is: %s", engGetLastErrorMessage());
    EngComponentDesc hd = healthDesc();
    EngComponent health = 0, again = 0;
    CHECK(engRegisterComponent(world, &hd, &health) == ENG_OK && health, "a kit's component, registered by schema");
    CHECK(engRegisterComponent(world, &hd, &again) == ENG_OK && again == health, "the same schema again: the same id");
    hd.revision = 1;
    CHECK(engRegisterComponent(world, &hd, &again) == ENG_ERROR_REFUSED && strstr(engGetLastErrorMessage(), "revision 0, now 1"),
          "a different revision is refused, naming both: %s", engGetLastErrorMessage());
    hd = healthDesc();
    hd.name = "core.Health";
    CHECK(engRegisterComponent(world, &hd, &again) == ENG_ERROR_REFUSED, "the engine's namespace is refused");
    {
        EngFieldDesc f[2] = { { "a", ENG_FIELD_F32, 0, 1, 0 }, { "b", ENG_FIELD_F32, 2, 1, 0 } };
        EngComponentDesc d = ENG_INIT(EngComponentDesc);
        d.name = "test.Overlap"; d.size = 8; d.align = 4; d.fields = f; d.field_count = 2;
        CHECK(engRegisterComponent(world, &d, &again) == ENG_ERROR_INVALID_ARGUMENT && strstr(engGetLastErrorMessage(), "overlaps"),
              "overlapping fields are refused: %s", engGetLastErrorMessage());
        f[1].offset = 4; f[1].type = ENG_FIELD_ASSET;
        d.name = "test.Asset";
        CHECK(engRegisterComponent(world, &d, &again) == ENG_ERROR_UNSUPPORTED, "an ASSET field is refused in this version: %s",
              engGetLastErrorMessage());
    }

    /* ── Entities ── */
    enum { N = 1000 };
    EngEntity es[N];
    int made = 1;
    for (int i = 0; i < N; ++i) {
        char name[16];
        snprintf(name, sizeof name, "e%d", i);
        EngTransform t = { { (float)i, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 } };
        made = made && engCreateEntity(world, name, &es[i]) == ENG_OK
                    && engAddComponent(world, es[i], transform, &t) == ENG_OK
                    && engAddComponent(world, es[i], health, NULL) == ENG_OK;
    }
    CHECK(made, "%d entities with a Transform and a Health", N);
    Health h;
    CHECK(engGetComponent(world, es[3], health, &h, sizeof h) == ENG_OK && h.current == 100.0f && h.maximum == 100.0f,
          "added with no value: the registered defaults");
    CHECK(engGetComponent(world, es[3], health, &h, 4) == ENG_ERROR_INVALID_ARGUMENT, "a wrong size is refused: %s",
          engGetLastErrorMessage());
    CHECK(engFindEntity(world, "e7") == es[7], "found by name");

    /* ── The change stream ── */
    EngChangeStream stream = 0;
    uint32_t kinds = 0;
    CHECK(engSubscribeChanges(world, health, &stream) == ENG_OK, "subscribed to Health");
    CHECK(drain(world, stream, ENG_CHANGE_ADDED, &kinds) == N && kinds == N, "a new stream reports what exists, as ADDED");
    h.current = 1.0f;
    CHECK(engSetComponent(world, es[9], health, &h, sizeof h) == ENG_OK, "a set");
    CHECK(drain(world, stream, ENG_CHANGE_CHANGED, &kinds) == 1 && kinds == 1, "is one CHANGED");

    /* ── A query: columns read and written in place ── */
    EngQueryTerm terms[2] = { { transform, ENG_ACCESS_READ, { 0, 0 } }, { health, ENG_ACCESS_WRITE, { 0, 0 } } };
    EngQueryDesc qd = ENG_INIT(EngQueryDesc);
    qd.term_count = 2;
    qd.terms = terms;
    EngQuery q = 0;
    CHECK(engCreateQuery(world, &qd, &q) == ENG_OK && q, "a query: Transform read, Health written");
    EngQueryIter* it = NULL;
    uint32_t rows = 0, batches = 0;
    int columnsOk = 1;
    CHECK(engQueryBegin(world, q, &it) == ENG_OK, "begun");
    EngQueryBatch b = ENG_INIT(EngQueryBatch);
    while (engQueryNext(it, &b)) {
        const EngTransform* t = (const EngTransform*)b.columns[0];
        Health* hp = (Health*)b.columns[1];
        columnsOk = columnsOk && t && hp && b.entities;
        for (uint32_t i = 0; i < b.count; ++i) hp[i].current = t[i].position.x;
        rows += b.count;
        ++batches;
    }
    engDestroyQuery(world, q);   /* refused while open: the query stays */
    engQueryEnd(it);
    CHECK(engQueryBegin(world, q, &it) == ENG_OK, "a destroy while it was open was refused, and it still runs");
    engQueryEnd(it);
    CHECK(rows == N && columnsOk, "every row, as columns (%u rows in %u batches)", rows, batches);
    CHECK(engGetComponent(world, es[42], health, &h, sizeof h) == ENG_OK && h.current == 42.0f,
          "written in place, through the column");
    CHECK(drain(world, stream, ENG_CHANGE_CHANGED, &kinds) == N && kinds == N, "WRITE: every row reported CHANGED");

    /* WRITE_MARKED: only what is marked. */
    EngQueryTerm marked = { health, ENG_ACCESS_WRITE_MARKED, { 0, 0 } };
    qd.term_count = 1;
    qd.terms = &marked;
    EngQuery qm = 0;
    CHECK(engCreateQuery(world, &qd, &qm) == ENG_OK, "a query with a marked write");
    uint32_t markedRows = 0;
    engQueryBegin(world, qm, &it);
    while (engQueryNext(it, &b)) {
        Health* hp = (Health*)b.columns[0];
        for (uint32_t i = 0; i < b.count; ++i) {
            if (hp[i].team != 0) continue;
            if (((uint32_t)hp[i].current) % 10 == 0) {     /* every tenth entity */
                hp[i].maximum = 50.0f;
                engMarkChanged(it, 0, i, 1);
                ++markedRows;
            }
        }
    }
    engQueryEnd(it);
    CHECK(drain(world, stream, ENG_CHANGE_CHANGED, &kinds) == markedRows && markedRows == N / 10,
          "WRITE_MARKED: only the %u marked rows reported", markedRows);

    /* WITHOUT, and structural changes during a query applied at its end. */
    EngEntity bare[10];
    for (int i = 0; i < 10; ++i) {
        EngTransform t = { { 0, 0, 0 }, { 0, 0, 0, 1 }, { 1, 1, 1 } };
        engCreateEntity(world, NULL, &bare[i]);
        engAddComponent(world, bare[i], transform, &t);
    }
    EngQueryTerm without[2] = { { transform, ENG_ACCESS_READ, { 0, 0 } }, { health, ENG_ACCESS_WITHOUT, { 0, 0 } } };
    qd.term_count = 2;
    qd.terms = without;
    EngQuery qw = 0;
    engCreateQuery(world, &qd, &qw);
    rows = 0;
    int nullColumn = 1;
    engQueryBegin(world, qw, &it);
    while (engQueryNext(it, &b)) {
        rows += b.count;
        nullColumn = nullColumn && b.columns[1] == NULL;
        for (uint32_t i = 0; i < b.count; ++i) engAddComponent(world, b.entities[i], health, NULL);
    }
    CHECK(rows == 10 && nullColumn, "WITHOUT: only the 10 with no Health, and no column for it");
    CHECK(!engHasComponent(world, bare[0], health), "a component added during the query waits for its end");
    engQueryEnd(it);
    CHECK(engHasComponent(world, bare[0], health), "and is there after it");
    CHECK(drain(world, stream, ENG_CHANGE_ADDED, &kinds) == 10 && kinds == 10, "the stream reports the 10 as ADDED");

    /* ── Stale handles ── */
    const EngEntity gone = es[5];
    engDestroyEntity(world, gone);
    CHECK(!engIsEntityAlive(world, gone), "destroyed: not alive");
    CHECK(engGetComponent(world, gone, health, &h, sizeof h) == ENG_ERROR_STALE_HANDLE, "a stale entity is refused: %s",
          engGetLastErrorMessage());
    CHECK(drain(world, stream, ENG_CHANGE_REMOVED, &kinds) == 1 && kinds == 1, "the stream reports it REMOVED");
    EngEntity fresh = 0;
    engCreateEntity(world, NULL, &fresh);
    CHECK(fresh != gone && !engIsEntityAlive(world, gone), "a new entity never has a destroyed one's handle");
    engDestroyQuery(world, qw);
    CHECK(engQueryBegin(world, qw, &it) == ENG_ERROR_STALE_HANDLE, "a destroyed query is refused");

    /* ── Parents and stable ids ── */
    CHECK(engSetEntityParent(world, es[1], es[0]) == ENG_OK && engGetEntityParent(world, es[1]) == es[0], "parented");
    CHECK(engSetEntityParent(world, es[0], es[1]) == ENG_ERROR_REFUSED, "a cycle is refused: %s", engGetLastErrorMessage());
    CHECK(engSetEntityParent(world, es[1], 0) == ENG_OK && engGetEntityParent(world, es[1]) == 0, "unparented");
    uint64_t sid[2] = { 0, 0 };
    CHECK(engGetEntityStableId(world, es[8], sid) == ENG_OK && sid[0] != 0 && engFindEntityByStableId(world, sid) == es[8],
          "a stable id, and the entity found by it");

    /* ── The engine steps with the kit's component in its world ── */
    int stepped = 1;
    for (int i = 0; i < 5; ++i) stepped = stepped && engStepWithDelta(engine, 1.0 / 60.0, NULL) == ENG_OK;
    CHECK(stepped, "the engine steps (its sim hash classifies the kit's component)");

    /* ── The handle released and taken again ── */
    engUnsubscribeChanges(world, stream);
    engDestroyWorld(world);
    CHECK(engCreateWorld(engine, NULL, &world) == ENG_OK, "the world, taken again");
    hd = healthDesc();
    CHECK(engRegisterComponent(world, &hd, &again) == ENG_OK && again == health && engIsEntityAlive(world, es[0]),
          "its components and entities are still there");

    engDestroyWorld(world);
    engDestroyEngine(engine);
    printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
