// ── skin_palette_test — the pool, and the leak it was built to have ─────────
//
// SkinPalettePool hands out 8 KB bone palettes by slot, so the ONE thing that
// matters beyond arithmetic is whether a slot ever comes back. Two failures are
// possible and they fail in opposite directions:
//
//   LEAK      a slot is never released, and 8 KB a time is invisible until a
//             session has spawned a few thousand skinned entities;
//   ALIASING  a slot is released TWICE, lands in the free list twice, and gets
//             handed to two entities — two skeletons writing one palette, which
//             looks like an animation bug in a completely unrelated system.
//
// The second is why `release` is idempotent and the first is why AnimatorSystem
// installs its remove hook on EVERY world it ticks. That second point is the
// reason this file exists rather than a comment: the fix depends on flecs firing
// on_remove during WORLD TEARDOWN, not just on an explicit remove or destruct,
// and that is an assumption about somebody else's library. Asserted here.
#include <cstdio>
#include <vector>

#include <flecs.h>

#include "animation/skin_palette.h"
#include "components/skinned_mesh.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// What AnimatorSystem::installReleaseHook does. Duplicated rather than included
// because pulling in the animator drags ozz and the registries into a test about
// a slot allocator — but it must stay identical, and the assertion below that a
// world WITHOUT the hook leaks is what proves the hook is what does the work.
static void installHook(flecs::world& w) {
    w.component<SkinnedMesh>().on_remove(
        [](flecs::entity, SkinnedMesh& s) {
            anim::skinPalettes().release(s.paletteSlot);
            s.paletteSlot = SkinnedMesh::kNoSlot;
        });
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("skin_palette_test\n");
    auto& pool = anim::skinPalettes();

    // ── The allocator itself ────────────────────────────────────────────────
    {
        const uint32_t a = pool.acquire();
        const uint32_t b = pool.acquire();
        CHECK(a != b, "two acquisitions get different slots (%u, %u)", a, b);
        CHECK(pool.at(a) != nullptr && pool.at(b) != nullptr,
              "both slots resolve to storage");
        CHECK(pool.at(a) != pool.at(b), "and the storage does not overlap");
        CHECK(pool.at(anim::SkinPalettePool::kNoSlot) == nullptr,
              "kNoSlot resolves to null, so an unanimated entity needs no case");

        // Chunk boundaries are where a slot-to-address calculation goes wrong,
        // and they are also where the chunk vector grows — the moment a lock-free
        // reader could see a half-published pointer.
        std::vector<uint32_t> slots;
        for (int i = 0; i < (int)anim::SkinPalettePool::kChunkSlots * 3; ++i)
            slots.push_back(pool.acquire());
        bool distinct = true, resolvable = true;
        for (size_t i = 0; i < slots.size(); ++i) {
            if (!pool.at(slots[i])) resolvable = false;
            for (size_t k = i + 1; k < slots.size(); ++k)
                if (pool.at(slots[i]) == pool.at(slots[k])) distinct = false;
        }
        CHECK(resolvable, "every slot across three chunks resolves");
        CHECK(distinct, "and no two share an address");

        // Writing the full palette through one slot must not touch its
        // neighbour — the off-by-one that a stride mistake produces.
        float* pa = pool.at(slots[0]);
        float* pb = pool.at(slots[1]);
        for (int i = 0; i < anim::SkinPalettePool::kFloats; ++i) pa[i] = 1.0f;
        for (int i = 0; i < anim::SkinPalettePool::kFloats; ++i) pb[i] = 2.0f;
        bool clean = true;
        for (int i = 0; i < anim::SkinPalettePool::kFloats; ++i)
            if (pa[i] != 1.0f) clean = false;
        CHECK(clean, "a full palette write stays inside its own slot");

        for (uint32_t s : slots) pool.release(s);
        pool.release(a); pool.release(b);
    }

    // ── Double release must be a no-op, not aliasing ────────────────────────
    {
        const size_t before = pool.slotsInUse();
        const uint32_t s = pool.acquire();
        pool.release(s);
        pool.release(s);           // the bug: two entries in the free list
        pool.release(s);
        const uint32_t x = pool.acquire();
        const uint32_t y = pool.acquire();
        CHECK(x != y, "after a triple release, two acquisitions still differ "
              "(%u, %u) — the slot did not enter the free list twice", x, y);
        pool.release(x); pool.release(y);
        CHECK(pool.slotsInUse() == before,
              "and the pool is back where it started (%zu)", pool.slotsInUse());
    }

    // Out-of-range and sentinel releases must not corrupt the free list.
    {
        const size_t before = pool.slotsInUse();
        pool.release(anim::SkinPalettePool::kNoSlot);
        pool.release(0xFFFFFFFEu);
        pool.release(anim::SkinPalettePool::kMaxSlots + 7);
        CHECK(pool.slotsInUse() == before,
              "releasing kNoSlot or an unallocated slot changes nothing");
    }

    // ── THE LEAK: does the hook fire when the WORLD dies? ───────────────────
    // AnimatorSystem installs the hook per world precisely because the play
    // snapshot world is created, animated and destroyed over and over. If flecs
    // did not fire on_remove during teardown, that fix would be worthless — so
    // this asserts the library behaviour the fix rests on, and asserts the
    // negative case in the same shape so a false pass is impossible.
    {
        const size_t baseline = pool.slotsInUse();
        constexpr int kEntities = 40;

        {   // WITH the hook: a destroyed world gives every slot back.
            flecs::world w;
            installHook(w);
            for (int i = 0; i < kEntities; ++i) {
                SkinnedMesh sm;
                sm.paletteSlot = pool.acquire();
                w.entity().set<SkinnedMesh>(sm);
            }
            CHECK(pool.slotsInUse() == baseline + kEntities,
                  "%d live entities hold %d slots", kEntities, kEntities);
        }
        CHECK(pool.slotsInUse() == baseline,
              "world teardown fires on_remove and returns every slot "
              "(%zu, baseline %zu)", pool.slotsInUse(), baseline);

        {   // An explicit destruct, the horde-shooter case.
            flecs::world w;
            installHook(w);
            flecs::entity e;
            SkinnedMesh sm;
            sm.paletteSlot = pool.acquire();
            e = w.entity().set<SkinnedMesh>(sm);
            e.destruct();
            CHECK(pool.slotsInUse() == baseline,
                  "destroying one entity returns its slot");
        }

        {   // WITHOUT the hook — the state this used to ship in. This is the
            // assertion that makes the two above mean something: the slots are
            // returned BY THE HOOK, not by anything flecs does on its own.
            flecs::world w;
            for (int i = 0; i < kEntities; ++i) {
                SkinnedMesh sm;
                sm.paletteSlot = pool.acquire();
                w.entity().set<SkinnedMesh>(sm);
            }
        }
        CHECK(pool.slotsInUse() == baseline + kEntities,
              "a world with NO hook leaks every slot (%zu vs baseline %zu) — "
              "which is what registering it on only the editor world did to "
              "every play session", pool.slotsInUse(), baseline);
    }

    if (g_failures) {
        std::printf("\nskin_palette_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("\nskin_palette_test: PASS\n");
    return 0;
}
