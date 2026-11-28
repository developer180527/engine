// eng_cpp_test — the C++ wrapper over the C world API (WO-095).
//
// Built with only include/ on its path (no src/, no flecs) and with
// -fno-exceptions -fno-rtti, as a game or kit may be: eng.hpp must need
// neither. Covers what the wrapper adds over the C calls it forwards:
//   - a component described once with ENG_COMPONENT: fields, offsets and
//     defaults from the type, the same description registered twice the same
//     id, a different one refused with the C API's message in the Result;
//   - entities as values: add with the default or a value, get, set, has,
//     remove, parents, destroy;
//   - Query<Read, Write, WriteMarked, With, Without>: columns typed per term,
//     and a WriteMarked column marking exactly the rows written through
//     write(i), contiguous or not, as the change stream reports them;
//   - systems over a query and without one, ordered by runAfter, their
//     functions owned and freed with them; RAII ending each handle.
#include <eng/eng.hpp>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <utility>
#include <vector>

namespace { int g_failures = 0; }
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("  (line %d: %s)\n", __LINE__, engGetLastErrorMessage()); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

struct Health {
    float    current = 100.0f;
    float    max = 100.0f;
    uint32_t team = 3;
    uint32_t pad = 0;
    EngEntity target = 0;
};
ENG_COMPONENT(Health, "cpptest.Health",
              eng::field("current", &Health::current),
              eng::field("max", &Health::max),
              eng::field("team", &Health::team),
              eng::field("pad", &Health::pad),
              eng::field("target", &Health::target, ENG_FIELD_ENTITY))

struct Tag { uint32_t v = 0; };
ENG_COMPONENT(Tag, "cpptest.Tag", eng::field("v", &Tag::v))

struct Frozen { uint32_t v = 0; };
ENG_COMPONENT(Frozen, "cpptest.Frozen", eng::field("v", &Frozen::v))

// Every kind of field the type deduces, and an array stated.
struct Kinds {
    EngVec3  position{1, 2, 3};
    EngQuat  rotation{0, 0, 0, 1};
    float    color[4] = {1, 0.5f, 0.25f, 1};
    char     label[16] = "kinds";
    int16_t  small = -2;
    uint8_t  flags = 7;
    uint8_t  pad[1] = {0};
    uint32_t ids[4] = {1, 2, 3, 4};
};
ENG_COMPONENT(Kinds, "cpptest.Kinds",
              eng::field("position", &Kinds::position),
              eng::field("rotation", &Kinds::rotation),
              eng::field("color", &Kinds::color),
              eng::field("label", &Kinds::label),
              eng::field("small", &Kinds::small),
              eng::field("flags", &Kinds::flags),
              eng::field("pad", &Kinds::pad, ENG_FIELD_U8),
              eng::field("ids", &Kinds::ids, ENG_FIELD_U32))

// Health again under the same name, laid out differently.
struct OtherHealth { double current = 0; };
ENG_COMPONENT(OtherHealth, "cpptest.Health", eng::field("current", &OtherHealth::current))

// The wrapper's columns are typed by access: a Read column cannot be written.
using HealthRead = decltype(std::declval<eng::Batch<eng::Read<Health>>&>().template column<0>());
using HealthWrite = decltype(std::declval<eng::Batch<eng::Write<Health>>&>().template column<0>());
using HealthMarked = decltype(std::declval<eng::Batch<eng::WriteMarked<Health>>&>().template column<0>());
static_assert(std::is_same_v<HealthRead, eng::Span<const Health>>, "Read: a span of const");
static_assert(std::is_same_v<HealthWrite, eng::Span<Health>>, "Write: a mutable span");
static_assert(std::is_same_v<HealthMarked, eng::MarkedColumn<Health>>, "WriteMarked: a column marked on write");
static_assert(std::is_same_v<decltype(std::declval<HealthMarked&>()[0]), const Health&>,
              "a WriteMarked column's [] is read-only: writing goes through write(i), which marks");

namespace {

struct Drained { uint32_t added = 0, changed = 0, removed = 0; std::vector<EngEntity> changedIds; };
Drained drain(eng::ChangeStream& s) {
    Drained d;
    auto r = s.read();
    if (!r) return d;
    for (const EngChange& c : *r) {
        if (c.kind == ENG_CHANGE_ADDED) ++d.added;
        if (c.kind == ENG_CHANGE_CHANGED) { ++d.changed; d.changedIds.push_back(c.entity); }
        if (c.kind == ENG_CHANGE_REMOVED) ++d.removed;
    }
    return d;
}

int g_order[4];
int g_orderN = 0;

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("eng_cpp_test: the C++ wrapper (WO-095)\n");

    auto engine = eng::Engine::create("eng_cpp_test");
    CHECK(engine.ok(), "an engine");
    auto owned = eng::OwnedWorld::create(*engine);
    CHECK(owned.ok(), "a world");
    eng::World w = **owned;

    std::printf("\n-- components --\n");
    auto health = w.component<Health>();
    CHECK(health.ok() && health->id, "Health, registered from its description");
    auto again = w.component<Health>();
    CHECK(again.ok() && again->id == health->id, "the same description again: the same id");
    auto other = w.component<OtherHealth>();
    CHECK(!other && other.code() == ENG_ERROR_REFUSED && !other.error().message.empty(),
          "another layout under the name: refused, the C API's reason in the Result: %s",
          other.error().message.c_str());
    auto kinds = w.component<Kinds>();
    CHECK(kinds.ok(), "every deduced field type, and arrays stated, register: %s",
          kinds ? "" : kinds.error().message.c_str());
    auto tag = w.component<Tag>();
    auto frozen = w.component<Frozen>();
    CHECK(tag.ok() && frozen.ok(), "two more");
    const auto& hf = eng::ComponentInfo<Health>::fields();
    CHECK(hf.size() == 5 && hf[4].offset == offsetof(Health, target) && hf[4].type == ENG_FIELD_ENTITY
              && hf[2].type == ENG_FIELD_U32 && hf[2].offset == 8,
          "offsets and types from the type itself");
    const auto& kf = eng::ComponentInfo<Kinds>::fields();
    CHECK(kf[2].type == ENG_FIELD_VEC4 && kf[3].type == ENG_FIELD_FIXED_STRING && kf[3].count == 16
              && kf[7].type == ENG_FIELD_U32 && kf[7].count == 4 && kf[6].count == 1,
          "float[4] a VEC4, char[16] a 16-byte string, uint32_t[4] four U32s");

    std::printf("\n-- entities --\n");
    auto e = w.createEntity("hero");
    CHECK(e.ok() && e->alive(), "an entity");
    CHECK(e->add(*health).ok(), "Health with its default");
    auto h = e->get(*health);
    CHECK(h.ok() && h->current == 100.0f && h->team == 3, "the default is the type's own: %g, team %u",
          h ? h->current : -1.0f, h ? h->team : 0u);
    Health hurt{};
    hurt.current = 40;
    CHECK(e->set(*health, hurt).ok() && e->get(*health)->current == 40.0f, "set, then get");
    CHECK(e->has(*health) && !e->has(*tag), "has");
    auto child = w.createEntity();
    CHECK(child->setParent(*e).ok() && child->parent() == *e, "a parent");
    CHECK(w.find("hero") == *e, "found by name");
    CHECK(child->add(*kinds).ok() && std::strcmp(child->get(*kinds)->label, "kinds") == 0
              && child->get(*kinds)->ids[3] == 4,
          "a fixed string and an array, by default");
    CHECK(e->remove(*health).ok() && !e->has(*health), "removed");
    auto missing = e->get(*health);
    CHECK(!missing && missing.code() != ENG_OK, "a get of what is not there: an error, with its message: %s",
          missing.error().message.c_str());
    child->destroy();
    CHECK(!child->alive(), "destroyed");
    auto bad = w.entity(child->id()).add(*tag);
    CHECK(!bad, "a stale entity: refused: %s", bad.error().message.c_str());

    std::printf("\n-- queries --\n");
    e->add(*health);
    eng::Entity ents[8];
    for (int i = 0; i < 8; ++i) {
        ents[i] = *w.createEntity();
        Health v{};
        v.current = (float)(10 * i);
        ents[i].add(*health, v);
        ents[i].add(*tag, Tag{(uint32_t)i});
    }
    ents[7].add(*frozen);
    {
        eng::Query<eng::Read<Tag>, eng::Write<Health>, eng::Without<Frozen>> q(w, *tag, *health, *frozen);
        CHECK(q, "Query<Read, Write, Without>");
        uint32_t rows = 0;
        q.each([&](auto& b) {
            auto t = b.template column<0>();
            auto hp = b.template column<1>();
            for (uint32_t i = 0; i < b.count(); ++i) { hp[i].team = t[i].v + 100; ++rows; }
        });
        CHECK(rows == 7 && ents[3].get(*health)->team == 103 && ents[7].get(*health)->team == 3,
              "columns in place: 7 rows (Frozen excluded, its team still the default), written: %u", rows);
        eng::Query<eng::With<Frozen>, eng::Read<Health>> fq(w, *frozen, *health);
        uint32_t frozenRows = 0;
        fq.each([&](auto& b) { frozenRows += b.count(); CHECK(b.entity(0) == ents[7], "the With row's entity"); });
        CHECK(frozenRows == 1, "With: only the frozen one");
    }
    {
        eng::Query<eng::Read<Health>> q;
        eng::Query<eng::Read<Health>> q2(w, eng::Component<Health>{});
        CHECK(!q2 && !q2.error().message.empty(), "a query on no component: not made, saying why: %s",
              q2.error().message.c_str());
        q = std::move(q2);
    }

    std::printf("\n-- WriteMarked: only what is written is reported --\n");
    {
        eng::ChangeStream s(w, *health);
        CHECK(s, "a change stream on Health");
        drain(s);   // the ADDED backlog
        eng::Query<eng::WriteMarked<Health>, eng::Read<Tag>> q(w, *health, *tag);
        // Rows 1, 2, 3 (contiguous: one mark) and 6 (another), by tag.
        q.each([&](auto& b) {
            auto hp = b.template column<0>();
            auto t = b.template column<1>();
            for (uint32_t i = 0; i < b.count(); ++i) {
                const uint32_t k = t[i].v;
                (void)hp[i].current;   // reading marks nothing
                if (k == 1 || k == 2 || k == 3 || k == 6) hp.write(i).current += 1;
                if (k == 2) hp.write(i).current += 1;   // the same row twice: still once
            }
        });
        Drained d = drain(s);
        bool right = d.changed == 4;
        for (EngEntity id : d.changedIds)
            right = right && (id == ents[1].id() || id == ents[2].id() || id == ents[3].id() || id == ents[6].id());
        CHECK(right, "four rows reported CHANGED, exactly those written: %u", d.changed);
        CHECK(ents[2].get(*health)->current == 22.0f && ents[6].get(*health)->current == 61.0f,
              "and the writes landed");

        eng::Query<eng::Write<Health>> all(w, *health);
        all.each([](auto& b) { auto hp = b.template column<0>(); (void)hp; });
        d = drain(s);
        CHECK(d.changed == 9, "a Write column marks every row, written or not: %u of 9", d.changed);
    }

    std::printf("\n-- systems --\n");
    {
        eng::Query<eng::WriteMarked<Health>, eng::Read<Tag>> q(w, *health, *tag);
        eng::SystemOptions late;
        late.name = "cpptest.second";
        late.phase = ENG_PHASE_FIXED_UPDATE;
        late.runAfter = {"cpptest.first"};
        uint64_t lastTick = 0;
        uint32_t healed = 0;
        auto second = eng::System::over(w, q, late, [&](const eng::Step& st, auto& b) {
            if (g_orderN < 4 && (g_orderN == 0 || g_order[g_orderN - 1] != 2)) g_order[g_orderN++] = 2;
            lastTick = st.tick;
            auto hp = b.template column<0>();
            for (uint32_t i = 0; i < b.count(); ++i)
                if (hp[i].current < 50) { hp.write(i).current += 1; ++healed; }
        });
        CHECK(second, "a system over a query, run after one not made yet: %s", second.error().message.c_str());
        eng::SystemOptions early;
        early.name = "cpptest.first";
        early.phase = ENG_PHASE_FIXED_UPDATE;
        int ran = 0;
        auto first = eng::System::every(w, early, [&](const eng::Step& st) {
            ++ran;
            if (g_orderN < 4) g_order[g_orderN++] = 1;
            CHECK(st.world.get() == w.get() && st.dt > 0, "the step's world and delta");
        });
        CHECK(first, "a system with no query");
        eng::ChangeStream s(w, *health);
        drain(s);
        engine->step(1.0 / 60.0);
        CHECK(ran == 1 && g_orderN >= 2 && g_order[0] == 1 && g_order[1] == 2 && lastTick >= 1,
              "one step: both ran, in runAfter's order");
        Drained d = drain(s);
        CHECK(d.changed == healed && healed == 5, "the system's marked writes reported: %u healed, %u changed", healed, d.changed);

        eng::SystemOptions dup;
        dup.name = "cpptest.first";
        auto clash = eng::System::every(w, dup, [](const eng::Step&) {});
        CHECK(!clash && !clash.error().message.empty(), "a name taken: not made, saying why: %s",
              clash.error().message.c_str());

        first.reset();
        engine->step(1.0 / 60.0);
        CHECK(ran == 1, "reset: it does not run again");
        // `second` ends with the scope, before its query does.
    }
    engine->step(1.0 / 60.0);
    std::printf("  ok    systems and queries ended at the end of their scope\n");

    owned->reset();
    engine->reset();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
