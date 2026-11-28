// ── eng_query_bench — what a query through the C API costs (WO-073) ─────────
// Not a test: numbers. 50 000 entities with core.Transform and a kit's
// component (registered through engRegisterComponent), spread over 8 tables as
// a real scene's archetypes spread them. One pass reads Transform and writes
// the kit's component, three ways:
//   flecs      ecs_query_iter / ecs_query_next on the same terms, directly
//   C API      engQueryBegin / engQueryNext / engQueryEnd
//   C API, watched   the same, with a change stream subscribed to the
//              written component, so ENG_ACCESS_WRITE marks every row
//   eng.hpp    the same pass through the C++ wrapper (WO-095): Query<Read,
//              Write>::each, typed columns; it must cost what the C API does
//   marked, C / eng.hpp   WRITE_MARKED, every row written, watched: one
//              engMarkChanged per batch by hand, against MarkedColumn::write
// Median of 51 passes each, after warm-up.
//   cmake --build build-rel --target eng_query_bench && build-rel/eng_query_bench
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include <eng/eng.hpp>
#include <flecs.h>

#include "capi/eng_internal.h"
#include "runtime/runtime.h"

struct Health { float current, maximum; };

template <class F> static double medianMs(F&& f) {
    for (int i = 0; i < 5; ++i) f();
    std::vector<double> ms;
    for (int i = 0; i < 51; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        f();
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    return ms[ms.size() / 2];
}

int main() {
    EngEngineDesc ed = ENG_INIT(EngEngineDesc);
    ed.api_version = ENG_API_VERSION;
    EngEngine* engine = nullptr;
    EngWorld* world = nullptr;
    if (engCreateEngine(&ed, &engine) != ENG_OK || engCreateWorld(engine, nullptr, &world) != ENG_OK) {
        std::printf("no engine: %s\n", engGetLastErrorMessage());
        return 1;
    }
    const EngFieldDesc fields[] = {{"current", ENG_FIELD_F32, 0, 1, 0}, {"maximum", ENG_FIELD_F32, 4, 1, 0}};
    EngComponentDesc d = ENG_INIT(EngComponentDesc);
    d.name = "bench.Health"; d.size = sizeof(Health); d.align = alignof(Health); d.fields = fields; d.field_count = 2;
    EngComponent health = 0;
    engRegisterComponent(world, &d, &health);
    const EngComponent transform = engFindComponent(world, "core.Transform");
    // Eight tags, to spread the entities over eight tables.
    EngComponent tags[8];
    for (int k = 0; k < 8; ++k) {
        char name[32]; std::snprintf(name, sizeof name, "bench.Kind%d", k);
        const EngFieldDesc f = {"v", ENG_FIELD_U32, 0, 1, 0};
        EngComponentDesc td = ENG_INIT(EngComponentDesc);
        td.name = name; td.size = 4; td.align = 4; td.fields = &f; td.field_count = 1;
        engRegisterComponent(world, &td, &tags[k]);
    }
    constexpr int N = 50000;
    for (int i = 0; i < N; ++i) {
        EngEntity e; engCreateEntity(world, nullptr, &e);
        EngTransform t = {{(float)i, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}};
        engAddComponent(world, e, transform, &t);
        engAddComponent(world, e, health, nullptr);
        engAddComponent(world, e, tags[i % 8], nullptr);
    }

    EngQueryTerm terms[2] = {{transform, ENG_ACCESS_READ, {0, 0}}, {health, ENG_ACCESS_WRITE, {0, 0}}};
    EngQueryDesc qd = ENG_INIT(EngQueryDesc);
    qd.term_count = 2; qd.terms = terms;
    EngQuery q = 0;
    engCreateQuery(world, &qd, &q);

    // The same terms, straight on flecs.
    flecs::world& fw = engInternalRuntime(engine)->simWorld();
    const flecs::entity_t trId = fw.id<Transform>();
    const flecs::entity_t hId = engInternalComponentId(world, health);
    ecs_query_desc_t fd{};
    fd.terms[0].id = trId; fd.terms[0].inout = EcsIn; fd.terms[0].src.id = EcsSelf;
    fd.terms[1].id = hId;  fd.terms[1].inout = EcsInOut; fd.terms[1].src.id = EcsSelf;
    fd.cache_kind = EcsQueryCacheAuto;
    ecs_query_t* fq = ecs_query_init(fw.c_ptr(), &fd);
    if (!hId || !fq) { std::printf("could not build the flecs query\n"); return 1; }

    volatile float sink = 0;
    auto viaFlecs = [&] {
        ecs_iter_t it = ecs_query_iter(fw.c_ptr(), fq);
        while (ecs_query_next(&it)) {
            const EngTransform* t = (const EngTransform*)ecs_field_w_size(&it, sizeof(EngTransform), 0);
            Health* h = (Health*)ecs_field_w_size(&it, sizeof(Health), 1);
            for (int i = 0; i < it.count; ++i) h[i].current = t[i].position.x * 0.5f;
        }
        sink = sink + 1;
    };
    uint32_t batches = 0;
    auto viaC = [&] {
        EngQueryIter* it = nullptr;
        engQueryBegin(world, q, &it);
        EngQueryBatch b = ENG_INIT(EngQueryBatch);
        batches = 0;
        while (engQueryNext(it, &b)) {
            const EngTransform* t = (const EngTransform*)b.columns[0];
            Health* h = (Health*)b.columns[1];
            for (uint32_t i = 0; i < b.count; ++i) h[i].current = t[i].position.x * 0.5f;
            ++batches;
        }
        engQueryEnd(it);
    };
    const double flecsMs = medianMs(viaFlecs);
    const double cMs = medianMs(viaC);
    EngChangeStream s = 0;
    engSubscribeChanges(world, health, &s);
    std::vector<EngChange> drainBuf(4096);
    auto viaCWatched = [&] {
        viaC();
        uint32_t n;
        do { n = (uint32_t)drainBuf.size(); engReadChanges(world, s, drainBuf.data(), &n); } while (n == drainBuf.size());
    };
    const double watchedMs = medianMs(viaCWatched);
    engUnsubscribeChanges(world, s);

    // The same pass through eng.hpp.
    const eng::World ew(world);
    const eng::Component<EngTransform> etr{transform};
    const eng::Component<Health> ehe{health};
    eng::Query<eng::Read<EngTransform>, eng::Write<Health>> wq(ew, etr, ehe);
    auto viaHpp = [&] {
        wq.each([](auto& b) {
            auto t = b.template column<0>();
            auto h = b.template column<1>();
            for (uint32_t i = 0; i < b.count(); ++i) h[i].current = t[i].position.x * 0.5f;
        });
    };
    const double hppMs = medianMs(viaHpp);

    // WRITE_MARKED, every row written, with a stream watching: by hand in C
    // (one mark per batch), and through MarkedColumn::write (coalesced to one).
    EngQueryTerm mterms[2] = {{transform, ENG_ACCESS_READ, {0, 0}}, {health, ENG_ACCESS_WRITE_MARKED, {0, 0}}};
    EngQueryDesc mqd = ENG_INIT(EngQueryDesc);
    mqd.term_count = 2; mqd.terms = mterms;
    EngQuery mq = 0;
    engCreateQuery(world, &mqd, &mq);
    eng::Query<eng::Read<EngTransform>, eng::WriteMarked<Health>> wmq(ew, etr, ehe);
    engSubscribeChanges(world, health, &s);
    auto drainAll = [&] {
        uint32_t n;
        do { n = (uint32_t)drainBuf.size(); engReadChanges(world, s, drainBuf.data(), &n); } while (n == drainBuf.size());
    };
    auto markedC = [&] {
        EngQueryIter* it = nullptr;
        engQueryBegin(world, mq, &it);
        EngQueryBatch b = ENG_INIT(EngQueryBatch);
        while (engQueryNext(it, &b)) {
            const EngTransform* t = (const EngTransform*)b.columns[0];
            Health* h = (Health*)b.columns[1];
            for (uint32_t i = 0; i < b.count; ++i) h[i].current = t[i].position.x * 0.5f;
            engMarkChanged(it, 1, 0, b.count);
        }
        engQueryEnd(it);
        drainAll();
    };
    auto markedHpp = [&] {
        wmq.each([](auto& b) {
            auto t = b.template column<0>();
            auto h = b.template column<1>();
            for (uint32_t i = 0; i < b.count(); ++i) h.write(i).current = t[i].position.x * 0.5f;
        });
        drainAll();
    };
    const double markedCMs = medianMs(markedC);
    const double markedHppMs = medianMs(markedHpp);
    engUnsubscribeChanges(world, s);
    engDestroyQuery(world, mq);

    std::printf("eng_query_bench: %d entities, %u batches (tables), median of 51\n", N, batches);
    std::printf("  flecs, direct        %7.3f ms  %5.2f ns/entity\n", flecsMs, flecsMs * 1e6 / N);
    std::printf("  C API                %7.3f ms  %5.2f ns/entity  (%+.3f ms)\n", cMs, cMs * 1e6 / N, cMs - flecsMs);
    std::printf("  C API, watched       %7.3f ms  %5.2f ns/entity  (WRITE marks every row; read back)\n",
                watchedMs, watchedMs * 1e6 / N);
    std::printf("  eng.hpp              %7.3f ms  %5.2f ns/entity  (%+.3f ms against the C API)\n",
                hppMs, hppMs * 1e6 / N, hppMs - cMs);
    std::printf("  marked, C            %7.3f ms  %5.2f ns/entity  (WRITE_MARKED, every row, watched)\n",
                markedCMs, markedCMs * 1e6 / N);
    std::printf("  marked, eng.hpp      %7.3f ms  %5.2f ns/entity  (%+.3f ms: MarkedColumn::write per row)\n",
                markedHppMs, markedHppMs * 1e6 / N, markedHppMs - markedCMs);
    ecs_query_fini(fq);
    engDestroyWorld(world);
    engDestroyEngine(engine);
    return 0;
}
