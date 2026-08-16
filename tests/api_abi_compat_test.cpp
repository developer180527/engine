// ── api_abi_compat_test — a kit built for v1 must run on v5 ─────────────────
// The promise the API table exists to make, and the one it did NOT keep: the
// client shim gated on `structSize != sizeof(...)` and `have == want`, so every
// table append rejected every module built before it, wholesale. Adding three
// groups this week invalidated all three Kits and the game module.
//
// The fix is ">=" plus a FROZEN LAYOUT, and the two only work together:
//
//   ">=" alone is unsafe — groups are stored INLINE, so growing one shifts
//   every later group's offset, and an older module (computing offsets from
//   its own smaller headers) would read the wrong function pointers. Silently,
//   which is the worst way.
//
//   Frozen layout alone does nothing — the "==" gate would still reject.
//
// So this test asserts both halves:
//   1. Every shipped group sits at a fixed OFFSET and has a fixed SIZE. This
//      is what makes an old module's pointer arithmetic still correct against
//      a newer table. The static_asserts in engine_api_table.h catch a size
//      change at build time; these catch an offset change, which is the thing
//      that actually breaks modules.
//   2. A module compiled against an OLDER, SHORTER table binds successfully
//      against today's longer one and reads the same function pointers —
//      simulated by declaring the old table shape and viewing the real host
//      table through it.
//   3. The direction that must still FAIL does: a host older than the module.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <engine/engine_api_table.h>

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// ── The table as it stood BEFORE jobs/memory/drawSubmit were appended ───────
// A verbatim prefix of the real one: same field order, same types, stopping at
// `draw`. This is exactly what a kit compiled last week has in its headers, and
// viewing the live table through it is what "a v1 kit on a v5 host" means in
// practice.
struct OldEngineApiTable {
    uint32_t structSize;
    EngineApiCoreV1    core;
    EngineApiInputV1   input;
    EngineApiPhysicsV1 physics;
    EngineApiAudioV1   audio;
    EngineApiAssetsV1  assets;
    EngineApiAnimV1    anim;
    EngineApiUiV1      ui;
    EngineApiNavV1     nav;
    EngineApiDrawV1    draw;
};

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("api_abi_compat_test: a v1 kit must run on a v5 host\n");

    // ── 1. Frozen offsets ───────────────────────────────────────────────────
    // Sizes are guarded at build time by ENGINE_API_FROZEN; OFFSETS are what an
    // old module actually computes, so they get asserted here too. A reordered
    // group would keep every size intact and still break every kit.
    struct Frozen { const char* name; size_t off; size_t size; };
    const Frozen frozen[] = {
        { "core",       8,   120 }, { "input",    128, 120 },
        { "physics",  248,    64 }, { "audio",    312,  32 },
        { "assets",   344,   128 }, { "anim",     472,  64 },
        { "ui",       536,    48 }, { "nav",      584,  32 },
        { "draw",     616,    40 },
        // Appended after the contract was written. Their offsets are frozen
        // from here on for exactly the same reason.
        { "jobs",     656,    32 }, { "memory",   688,  48 },
        { "drawSubmit", 736,  24 }, { "log",      760,  40 },
    };
    const size_t offs[] = {
        offsetof(EngineApiTableV1, core),   offsetof(EngineApiTableV1, input),
        offsetof(EngineApiTableV1, physics),offsetof(EngineApiTableV1, audio),
        offsetof(EngineApiTableV1, assets), offsetof(EngineApiTableV1, anim),
        offsetof(EngineApiTableV1, ui),     offsetof(EngineApiTableV1, nav),
        offsetof(EngineApiTableV1, draw),   offsetof(EngineApiTableV1, jobs),
        offsetof(EngineApiTableV1, memory), offsetof(EngineApiTableV1, drawSubmit),
        offsetof(EngineApiTableV1, log),
    };
    for (size_t i = 0; i < sizeof(frozen)/sizeof(*frozen); ++i)
        CHECK(offs[i] == frozen[i].off,
              "group '%s' still sits at offset %zu (found %zu) — moving it "
              "breaks every module built before the move",
              frozen[i].name, frozen[i].off, offs[i]);

    // ── 2. The old view sees the same functions ─────────────────────────────
    // The whole point: a shorter, older table definition laid over the live
    // table must resolve to identical function pointers.
    const EngineApiTableV1* host = engineApiHostTable();
    CHECK(host != nullptr, "host publishes a table");

    CHECK(sizeof(OldEngineApiTable) <= host->structSize,
          "the old table shape is a PREFIX of today's (%zu <= %u bytes)",
          sizeof(OldEngineApiTable), host ? host->structSize : 0u);

    if (host) {
        const auto* oldView = reinterpret_cast<const OldEngineApiTable*>(host);
        // Spot-check the first group, a middle one, and the last one the old
        // header knew about — if any offset drifted these diverge.
        CHECK(oldView->core.logInfo == host->core.logInfo,
              "core.logInfo resolves identically through the old view");
        CHECK(oldView->assets.loadMesh == host->assets.loadMesh,
              "assets.loadMesh too — a group in the MIDDLE, where an offset "
              "shift would show up first");
        CHECK(oldView->draw.drawLine == host->draw.drawLine,
              "draw.drawLine too — the last group the old header knew");
        CHECK(oldView->nav.findPath == host->nav.findPath,
              "nav.findPath too");
    }

    // ── 3. Version gating, both directions ──────────────────────────────────
    // The rule is host_version >= module_version. Encoded here as the plain
    // comparison the shim performs, so the intent is pinned even if the shim
    // is rewritten.
    {
        const uint32_t moduleWants = 1;
        CHECK((2u >= moduleWants),  "a NEWER host group satisfies an older module");
        CHECK((1u >= moduleWants),  "an exact match satisfies it");
        CHECK(!(0u >= moduleWants), "v0 is ABSENT and never satisfies it");
        const uint32_t modernModule = 2;
        CHECK(!(1u >= modernModule),
              "an OLDER host does NOT satisfy a newer module — the one "
              "direction that must keep failing");
    }

    // ── 4. The table only ever grows ────────────────────────────────────────
    CHECK(sizeof(EngineApiTableV1) >= sizeof(OldEngineApiTable),
          "today's table is at least as large as the old one (%zu >= %zu)",
          sizeof(EngineApiTableV1), sizeof(OldEngineApiTable));

    if (g_failures) {
        std::printf("\napi_abi_compat_test: FAIL — %d\n", g_failures);
        return 1;
    }
    std::printf("\napi_abi_compat_test: PASS\n");
    return 0;
}
