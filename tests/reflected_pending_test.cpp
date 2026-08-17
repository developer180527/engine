// ── reflected_pending_test — a scene must outlive the kit that authored it ────
//
// The failure this guards is the one that ends plugin ecosystems, and vCAD's
// PLUGIN_CONTRACT.md §4A names it exactly: you open a colleague's file on a
// machine without the plugin that made it, the unknown components are dropped,
// you save, and their work is gone.
//
// `reflected_serde.h` already has the design — an unresolved component blob is
// stashed in `ReflectedPending` and re-emitted on save. What was never tested is
// **the save**, and that is the only step that can destroy anything. A load that
// stashes correctly and a save that silently omits the stash look identical from
// inside the ignorant session: the scene opens, nothing warns, and the damage is
// only visible to the person who had the kit.
//
// So every case here goes THROUGH a session that cannot understand the data and
// saves from it. Hermetic: "the type is not registered" is the entire condition,
// so this needs no dlopen, no kit, and no project — three flecs worlds is the
// whole fixture.
#include <cstdio>
#include <cstring>
#include <string>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "scene/reflected_serde.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// Stand-ins for a kit's components. Two of them, because one unknown type can be
// preserved by luck; a map that loses its second entry cannot.
namespace kitsim {
struct Widget { float length; float ratio; int32_t count; };
struct Gadget { float mass; uint32_t flags; };
}
// A component the HOST always knows, to prove the two paths do not interfere.
namespace hostsim {
struct Marker { int32_t id; };
}

static void registerWidget(flecs::world& w) {
    w.component<kitsim::Widget>()
        .member<float>("length").member<float>("ratio").member<int32_t>("count");
}
static void registerGadget(flecs::world& w) {
    w.component<kitsim::Gadget>().member<float>("mass").member<uint32_t>("flags");
}
static void registerMarker(flecs::world& w) {
    w.component<hostsim::Marker>().member<int32_t>("id");
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("reflected_pending_test\n");

    // Values chosen to be awkward on purpose. A float that survives a JSON
    // round-trip only if the writer emits enough digits is exactly the kind of
    // loss that looks like nothing: the scene opens, the number is *almost*
    // right, and nobody can say which save damaged it.
    const float kLength = 0.1f;                 // not representable in binary
    const float kRatio  = 1.0f / 3.0f;
    const int32_t kCount = -1234567;
    const float kMass   = 1.7014118e38f;        // near FLT_MAX
    const uint32_t kFlags = 0xDEADBEEFu;

    // ── 1. Author WITH the kit ──────────────────────────────────────────────
    nlohmann::json authored;
    {
        flecs::world w;
        registerMarker(w); registerWidget(w); registerGadget(w);
        flecs::entity e = w.entity("Thing");
        e.set<hostsim::Marker>({ 7 });
        e.set<kitsim::Widget>({ kLength, kRatio, kCount });
        e.set<kitsim::Gadget>({ kMass, kFlags });
        reflected::save(e, authored);

        CHECK(authored.contains("kitsim::Widget") && authored.contains("kitsim::Gadget"),
              "authoring with the kit emits both components");
        CHECK(authored.contains("hostsim::Marker"),
              "and the host's own reflected component");
    }

    // ── 2. THE DANGEROUS STEP: open without the kit, then SAVE ──────────────
    nlohmann::json fromIgnorantSession;
    {
        flecs::world w;
        registerMarker(w);                      // host knows this one
        // Widget and Gadget are NOT registered — this is the colleague's machine.
        flecs::entity e = w.entity("Thing");
        reflected::load(e, authored);

        CHECK(e.has<reflected::ReflectedPending>(),
              "an unknown component is STASHED, not dropped");
        CHECK(e.get<reflected::ReflectedPending>().blobs.size() == 2,
              "both unknown components are stashed (%zu)",
              e.get<reflected::ReflectedPending>().blobs.size());
        CHECK(e.has<hostsim::Marker>() && e.get<hostsim::Marker>().id == 7,
              "while the component this session DOES understand applies normally");

        // The step nothing tested. If this omits the stash, the file is damaged
        // and the session that damaged it cannot tell.
        reflected::save(e, fromIgnorantSession);

        CHECK(fromIgnorantSession.contains("kitsim::Widget"),
              "SAVING from a session without the kit RE-EMITS the unknown data");
        CHECK(fromIgnorantSession.contains("kitsim::Gadget"),
              "...both of them, not just the first");
        CHECK(fromIgnorantSession.contains("hostsim::Marker"),
              "...alongside what it did understand");
        CHECK(fromIgnorantSession == authored,
              "and the result is byte-identical to what the kit wrote:\n         %s\n         %s",
              fromIgnorantSession.dump().c_str(), authored.dump().c_str());
    }

    // ── 3. Reopen WITH the kit — the values must be exactly the originals ────
    {
        flecs::world w;
        registerMarker(w); registerWidget(w); registerGadget(w);
        flecs::entity e = w.entity("Thing");
        reflected::load(e, fromIgnorantSession);

        CHECK(e.has<kitsim::Widget>() && e.has<kitsim::Gadget>(),
              "reinstalling the kit restores both components");
        CHECK(!e.has<reflected::ReflectedPending>(),
              "and nothing is left pending");

        const kitsim::Widget& wd = e.get<kitsim::Widget>();
        const kitsim::Gadget& gd = e.get<kitsim::Gadget>();
        // EXACT equality, deliberately. "close enough" is how a document quietly
        // drifts every time it passes through a machine missing a plugin.
        CHECK(wd.length == kLength,
              "float survives the ignorant round-trip EXACTLY (%.9g vs %.9g)",
              (double)wd.length, (double)kLength);
        CHECK(wd.ratio == kRatio, "and so does 1/3 (%.9g vs %.9g)",
              (double)wd.ratio, (double)kRatio);
        CHECK(wd.count == kCount, "signed int intact (%d)", wd.count);
        CHECK(gd.mass == kMass, "a near-FLT_MAX float intact (%.9g vs %.9g)",
              (double)gd.mass, (double)kMass);
        CHECK(gd.flags == kFlags, "a full-range uint32 intact (0x%X)", gd.flags);
    }

    // ── 4. Two ignorant round-trips must not degrade further ────────────────
    // Once through could preserve by accident; a format that loses a little each
    // pass is the version that destroys a file over a week of collaboration.
    {
        nlohmann::json pass = authored;
        for (int i = 0; i < 5; ++i) {
            flecs::world w;
            registerMarker(w);
            flecs::entity e = w.entity("Thing");
            reflected::load(e, pass);
            nlohmann::json out;
            reflected::save(e, out);
            pass = out;
        }
        CHECK(pass == authored,
              "five consecutive saves from kit-less sessions change nothing");
    }

    // ── 5. A PARTIAL restore keeps the rest pending ─────────────────────────
    // The realistic case: a colleague has one of the two kits. The one they lack
    // must still survive their save.
    {
        flecs::world w;
        registerMarker(w); registerWidget(w);    // Gadget still unknown
        flecs::entity e = w.entity("Thing");
        reflected::load(e, authored);

        CHECK(e.has<kitsim::Widget>(), "the kit they have applies");
        CHECK(e.has<reflected::ReflectedPending>() &&
              e.get<reflected::ReflectedPending>().blobs.count("kitsim::Gadget") == 1,
              "the kit they lack stays pending");

        nlohmann::json out;
        reflected::save(e, out);
        CHECK(out == authored,
              "and a save from that half-equipped session still writes everything");
    }

    // ── 6. applyPending is what makes a mid-session kit load work ───────────
    {
        flecs::world w;
        registerMarker(w);
        flecs::entity e = w.entity("Thing");
        reflected::load(e, authored);
        CHECK(e.has<reflected::ReflectedPending>(), "starts pending");

        // The kit arrives (Play starts, or the Plug-in Manager loads it).
        registerWidget(w);
        reflected::applyPending(w);
        CHECK(e.has<kitsim::Widget>() && e.get<kitsim::Widget>().count == kCount,
              "applyPending applies what now resolves");
        CHECK(e.has<reflected::ReflectedPending>() &&
              e.get<reflected::ReflectedPending>().blobs.count("kitsim::Widget") == 0,
              "and drops only that blob from the stash");
        CHECK(e.get<reflected::ReflectedPending>().blobs.count("kitsim::Gadget") == 1,
              "leaving the still-unknown one alone");

        registerGadget(w);
        reflected::applyPending(w);
        CHECK(e.has<kitsim::Gadget>() && !e.has<reflected::ReflectedPending>(),
              "and the stash is removed entirely once everything resolves");
    }

    // ── 7. A malformed blob must not lose the entity ────────────────────────
    // A hand-edited scene, or a kit that changed its schema. Keeping defaults and
    // warning is the documented behaviour; losing the whole entity is not.
    {
        flecs::world w;
        registerMarker(w); registerWidget(w);
        flecs::entity e = w.entity("Thing");
        nlohmann::json bad;
        bad["kitsim::Widget"] = "not an object at all";
        bad["hostsim::Marker"] = { {"id", 3} };
        reflected::load(e, bad);
        CHECK(e.has<hostsim::Marker>() && e.get<hostsim::Marker>().id == 3,
              "a bad blob does not stop the rest of the entity loading");
    }

    if (g_failures) {
        std::printf("\nreflected_pending_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("\nreflected_pending_test: PASS\n");
    return 0;
}
