// cpp_module_regen: a C++ kit for cpp_kit_host_test (WO-095).
//
// Written against eng.hpp alone: only include/eng/ is on its path, it links
// nothing of the engine's, and it is built with -fno-exceptions -fno-rtti.
// It describes the combat kit's Health itself (the C kit owns it; the engine
// checks this description against the one registered, field by field, with
// the defaults), adds its own regen.Rate, and each simulation step heals every
// hurt Health by its Rate up to the max, through a WriteMarked column, so only
// the entities it heals are reported changed.
#define ENG_CLIENT_IMPLEMENTATION
#include <eng/eng_client.h>
#include <eng/eng.hpp>

namespace {

// The combat kit's combat.Health, as this kit sees it (samples/combat_c/combat.h).
struct Health { float current = 100.0f, max = 100.0f; };
struct Rate { float per_step = 1.0f; uint32_t starts = 0; };

}  // namespace

ENG_COMPONENT(Health, "combat.Health",
              eng::field("current", &Health::current),
              eng::field("max", &Health::max))
ENG_COMPONENT(Rate, "regen.Rate",
              eng::field("per_step", &Rate::per_step),
              eng::field("starts", &Rate::starts))

namespace {

struct RegenKit {
    eng::Query<eng::WriteMarked<Health>, eng::Read<Rate>> query;
    eng::System system;

    eng::Result<void> start(eng::World w) {
        auto health = w.component<Health>();
        if (!health) return health.error();
        auto rate = w.component<Rate>();
        if (!rate) return rate.error();
        query = eng::Query<eng::WriteMarked<Health>, eng::Read<Rate>>(w, *health, *rate);
        if (!query) return query.error();
        eng::SystemOptions o;
        o.name = "regen.heal";
        o.phase = ENG_PHASE_FIXED_UPDATE;
        system = eng::System::over(w, query, o, [](const eng::Step&, auto& b) {
            auto hp = b.template column<0>();
            auto r = b.template column<1>();
            for (uint32_t i = 0; i < b.count(); ++i) {
                if (hp[i].current >= hp[i].max) continue;
                Health& h = hp.write(i);
                h.current = h.current + r[i].per_step < h.max ? h.current + r[i].per_step : h.max;
            }
        });
        if (!system) return system.error();
        // The starts are counted where the host can read them, on "regen": the
        // kit itself is loaded afresh at each Play.
        eng::Entity e = w.find("regen");
        if (!e) {
            auto made = w.createEntity("regen");
            if (!made) return made.error();
            e = *made;
            Rate none{};
            none.per_step = 0;
            e.add(*rate, none);
        }
        auto cur = e.get(*rate);
        if (cur) { ++cur->starts; e.set(*rate, *cur); }
        return {};
    }
    void stop(eng::World) {
        system.reset();   // before the query it runs over
        query.reset();
    }
};

}  // namespace

ENG_MODULE_EXPORT const EngModuleDesc* engModuleEntry(EngGetProcFn get_proc) {
    if (engClientLoad(get_proc, nullptr) != ENG_OK) return nullptr;
    return eng::module<RegenKit>("regen", "1.0");
}
