// ── fuzz_scene_service_test — the CONSUMER of the cooked scene ───────────────
// `fuzz_scene_loader_test` proves `assetlib::loadScene` turns hostile bytes into
// a SceneAsset or a clean rejection. That is the parse. This target covers what
// `src/runtime` does with the result, which is a different and larger surface:
// entity creation, id-collision resolution, string-table dereferences, asset
// resolution through AssetService, parent linking, and the load/unload lifecycle.
//
// It exists because `src/runtime` could not honestly claim `hardened` on the
// parse fuzzer alone. A parse that returns safely and a CONSUMER that leaks
// entities, builds a parent cycle, or leaves half a scene behind on a rejected
// load are independent failures, and only this target can see the second kind.
//
// Real everything: a real `AssetService` over a real `AssetRegistry`, a real
// flecs world, and real cooked meshes on disk. bgfx runs on the **Noop**
// backend, which executes nothing but exercises every handle path — the same
// arrangement `mesh_dedup_test` uses. So the GPU-resource bookkeeping under
// load/unload is real bookkeeping, not a stub.
//
// Properties asserted per case:
//   1. loadScene never throws and never crashes, on any file.
//   2. A REJECTED load leaves nothing behind. Returning 0 after having already
//      spawned half the entities is a leak that no parse test can detect.
//   3. load → unload is BALANCED: the world returns to its exact baseline
//      entity count. This is the property that makes level streaming possible.
//   4. Parent links terminate. `parentId` is attacker-controlled, so self-parent,
//      mutual pairs and long chains all arrive here; `safeReparent` is the
//      guard and this is what proves it holds.
//   5. Handle hygiene: unloading a bogus or already-unloaded handle is false,
//      not a crash, and never disturbs a live scene.
#include "fuzz/fuzz.h"

#include "runtime/services/scene_service.h"
#include "runtime/services/asset_service.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "render/vertex.h"
#include "components/entity_id.h"

#include <assetlib/scene_asset.h>
#include <assetlib/mesh_asset.h>

#include <bgfx/bgfx.h>
#include <flecs.h>

#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static constexpr uint32_t kGeneratorVersion = 1;

namespace {

// Set up once, in main: the scenes reference these, so the asset path is
// genuinely exercised instead of failing at the first fopen.
fs::path g_root;
std::vector<std::string> g_realMeshes;

// A minimal but VALID cooked mesh. Written through the real writer so its
// layout cannot drift from what the loader expects.
bool writeRealMesh(const fs::path& out) {
    assetlib::MeshAsset m;
    m.header.version      = 4;
    m.header.vertexFlags  = assetlib::VF_POSITION | assetlib::VF_NORMAL
                          | assetlib::VF_UV0 | assetlib::VF_TANGENT;
    m.header.vertexStride = sizeof(Vertex);
    m.header.vertexCount  = 3;
    m.header.indexStride  = 4;
    m.header.indexCount   = 3;
    m.vertexData.assign((size_t)m.header.vertexCount * m.header.vertexStride, 0);
    const uint32_t idx[3] = { 0, 1, 2 };
    m.indexData.assign((const uint8_t*)idx, (const uint8_t*)idx + sizeof(idx));
    // Give it a real extent so nothing downstream divides by a zero bound.
    for (int i = 0; i < 3; ++i) { m.header.boundsMin[i] = -1.0f; m.header.boundsMax[i] = 1.0f; }
    return assetlib::saveMesh(m, out);
}

// A scene of the shape SceneService actually consumes, with the identity and
// parent fields deliberately hostile — those are the parts the parse fuzzer
// hands over untouched and only the consumer interprets.
assetlib::SceneAsset buildScene(fuzz::Rng& rng) {
    using namespace assetlib;
    SceneAsset s;

    const uint32_t n = rng.range(0, 40);
    std::vector<uint64_t> issuedIds;

    for (uint32_t i = 0; i < n; ++i) {
        SceneEntity e{};

        // Identity: duplicates and zero are the interesting cases. A cooked
        // scene with two entities claiming one id must resolve, not alias.
        switch (rng.below(5)) {
            case 0:  e.entityId = 0; break;                       // "no id"
            case 1:  e.entityId = issuedIds.empty() ? rng.next()  // duplicate
                                : issuedIds[rng.below((uint32_t)issuedIds.size())]; break;
            case 2:  e.entityId = 1; break;                       // collides with anything
            default: e.entityId = rng.next(); break;
        }
        issuedIds.push_back(e.entityId);

        // Parenting: this is the adversarial part. safeReparent is the only
        // thing standing between these and an infinite ancestor walk.
        switch (rng.below(6)) {
            case 0:  e.parentId = 0; break;                       // root
            case 1:  e.parentId = e.entityId; break;              // SELF-parent
            case 2:  e.parentId = issuedIds.empty() ? 0           // a real ancestor
                                : issuedIds[rng.below((uint32_t)issuedIds.size())]; break;
            case 3:  e.parentId = rng.next(); break;              // dangling
            // A deliberate chain: each entity parented to the PREVIOUS one, so
            // a 40-entity scene builds a 40-deep hierarchy. Combined with the
            // self-parent and duplicate-id cases above, this is how a cycle
            // gets built out of individually reasonable records.
            default: e.parentId = i ? issuedIds[i - 1] : 0; break;
        }

        uint32_t mask = kComp_Transform;
        if (rng.chance(70)) mask |= kComp_Name;
        if (rng.chance(65)) mask |= kComp_MeshRenderer;
        if (rng.chance(15)) mask |= kComp_Camera;
        if (rng.chance(25)) mask |= kComp_RigidBody;
        if (rng.chance(20)) mask |= kComp_Script;
        if (rng.chance(10)) mask |= kComp_CharacterController;
        if (rng.chance(20)) mask |= kComp_Light;
        if (rng.chance(15)) mask |= kComp_Animator;
        if (rng.chance(8))  mask |= (1u << rng.range(9, 31));   // a bit from a later version
        e.componentMask = mask;

        auto put = [&](const std::string& v, uint32_t& off, uint32_t& len) {
            const auto [o, l] = stringTableAppend(s.stringTable, v);
            off = o; len = l;
        };
        auto junk = [&](uint32_t maxLen) {
            std::string v;
            const uint32_t k = rng.range(0, maxLen);
            for (uint32_t c = 0; c < k; ++c) v.push_back((char)rng.range(32, 126));
            return v;
        };

        if (mask & kComp_Name) put(junk(40), e.nameOffset, e.nameLength);
        if (mask & kComp_MeshRenderer) {
            // Half the time a mesh that really loads — otherwise the GPU and
            // residency paths would never run and this would be a parse test
            // with extra steps.
            if (rng.chance(50) && !g_realMeshes.empty())
                put(g_realMeshes[rng.below((uint32_t)g_realMeshes.size())],
                    e.meshCookedOffset, e.meshCookedLength);
            else
                put(junk(60), e.meshCookedOffset, e.meshCookedLength);

            if (rng.chance(40)) put(junk(40), e.meshSourceOffset, e.meshSourceLength);
            e.meshSourceType = (uint8_t)rng.below(2);
            if (rng.chance(40)) {
                const auto [o, l] = stringTableAppend(s.stringTable, junk(32));
                (void)l;
                e.materialNameOffset = o;       // z-terminated, no length field
            }
            // Sentinel and out-of-range offsets: the consumer dereferences
            // these, so it is the consumer that has to survive them.
            if (rng.chance(12)) e.meshCookedOffset  = rng.interestingU32();
            if (rng.chance(10)) e.materialNameOffset = rng.interestingU32();
            if (rng.chance(10)) e.meshCookedLength  = rng.interestingU32();
        }
        if (mask & kComp_Script)   put(junk(50), e.scriptPathOffset, e.scriptPathLength);
        if (mask & kComp_Animator) {
            if (rng.chance(50)) put(junk(40), e.animClipPathOffset, e.animClipPathLength);
            e.animClipIndex = (int16_t)rng.next();
            e.animPlaying   = (uint8_t)rng.below(2);
        }

        // Enum-ish bytes well outside their declared ranges.
        e.lightType        = (uint8_t)rng.below(255);
        e.rbBodyType       = (uint8_t)rng.below(255);
        e.rbShape          = (uint8_t)rng.below(255);
        e.cameraProjection = (uint8_t)rng.below(255);

        for (int k = 0; k < 3; ++k) e.position[k] = (float)((int32_t)rng.next() % 1000);
        for (int k = 0; k < 3; ++k) e.scale[k]    = rng.chance(10) ? 0.0f : 1.0f;

        s.entities.push_back(e);
    }
    s.header.entityCount     = (uint32_t)s.entities.size();
    s.header.stringTableSize = (uint32_t)s.stringTable.size();
    return s;
}

// Walk to the root. Returns false if the chain does not terminate within a
// generous bound, which is what a cycle looks like from the outside.
bool ancestorChainTerminates(flecs::entity e, int limit = 512) {
    flecs::entity cur = e;
    for (int i = 0; i < limit; ++i) {
        flecs::entity p = cur.target(flecs::ChildOf);
        if (!p || !p.is_alive()) return true;
        if (p == cur) return false;              // self-parent survived
        cur = p;
    }
    return false;
}

uint32_t liveEntityCount(flecs::world& w) {
    uint32_t n = 0;
    w.query<const EntityId>().each([&](flecs::entity, const EntityId&) { ++n; });
    return n;
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "scene_service";

    fuzz::Rng rng(fuzz::deriveSeed(masterSeed, "scene_service"));

    // A fresh world per case: entity-id state must not leak between cases or a
    // repro by seed stops being a repro.
    flecs::world world;
    AssetRegistry    meshes;
    TextureRegistry  textures;
    MaterialRegistry materials;
    AssetService assets({meshes, textures, materials});
    assets.setProjectRoot(g_root);

    SceneService svc({assets, meshes, textures, materials, world});
    svc.setCacheRoot(g_root / ".cache");

    const uint32_t baseline = liveEntityCount(world);

    const fs::path file = g_root / ".cache" / "scenes"
                        / ("fuzz-" + std::to_string(fuzz::currentPid()) + ".scene");
    {
        const auto scene = buildScene(rng);
        if (!assetlib::saveScene(scene, file)) return;
        // Truncate sometimes: the consumer must handle a rejection from the
        // parse layer as cleanly as it handles a success.
        if (rng.chance(15)) {
            std::vector<uint8_t> bytes;
            { std::ifstream in(file, std::ios::binary);
              bytes.assign(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()); }
            bytes.resize(bytes.empty() ? 0 : rng.below((uint32_t)bytes.size()));
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        }
    }

    // ── Property 1: the call under test must not throw ──────────────────────
    uint32_t handle = 0;
    try {
        handle = svc.loadScene(file.string().c_str());
    } catch (const std::exception& ex) {
        rep.fail(key, std::string("loadScene THREW: ") + ex.what());
        return;
    } catch (...) {
        rep.fail(key, "loadScene threw a non-std exception");
        return;
    }

    const uint32_t afterLoad = liveEntityCount(world);

    if (handle == 0) {
        // ── Property 2: a rejection leaves NOTHING behind ───────────────────
        if (afterLoad != baseline)
            rep.fail(key, "loadScene returned 0 but left "
                     + std::to_string(afterLoad - baseline) + " entit(ies) in the "
                     "world — a failed load must not spawn a partial scene");
        return;
    }

    // ── Property 4: every parent link terminates ────────────────────────────
    bool cyclic = false;
    world.query<const EntityId>().each([&](flecs::entity e, const EntityId&) {
        if (!cyclic && !ancestorChainTerminates(e)) cyclic = true;
    });
    if (cyclic) {
        rep.fail(key, "an entity's ancestor chain does not terminate — a cooked "
                      "scene built a parent CYCLE, and every transform walk over "
                      "it hangs");
        return;
    }

    // ── Property 5: handle hygiene ──────────────────────────────────────────
    if (svc.unloadScene(handle + 1000))
        rep.fail(key, "unloadScene accepted a handle that was never issued");
    // The bookkeeping the handle indexes: a live scene must report the entity
    // count it actually spawned, or unload cannot know what to destroy.
    if (svc.sceneEntityCount(handle) != afterLoad - baseline)
        rep.fail(key, "sceneEntityCount reports "
                 + std::to_string(svc.sceneEntityCount(handle)) + " but the world "
                 "gained " + std::to_string(afterLoad - baseline)
                 + " — unload destroys from this list, so a disagreement is a leak "
                   "or a double-destroy waiting to happen");

    // ── Property 3: load/unload is balanced ─────────────────────────────────
    if (!svc.unloadScene(handle)) {
        rep.fail(key, "unloadScene refused the handle loadScene just returned");
        return;
    }
    const uint32_t afterUnload = liveEntityCount(world);
    if (afterUnload != baseline) {
        rep.fail(key, "load/unload LEAKED: baseline " + std::to_string(baseline)
                 + ", after load " + std::to_string(afterLoad) + ", after unload "
                 + std::to_string(afterUnload) + " — streaming a level in and out "
                 "grows the world without bound");
        return;
    }
    if (svc.unloadScene(handle))
        rep.fail(key, "unloadScene succeeded TWICE on one handle");

    std::error_code ec;
    fs::remove(file, ec);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // Noop: no window, no driver, but every bgfx handle path runs for real —
    // which is what makes the load/unload resource bookkeeping meaningful.
    bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width = 16;
    init.resolution.height = 16;
    if (!bgfx::init(init)) { std::printf("FAIL bgfx init\n"); return 1; }
    bgfx::frame();

    g_root = fs::temp_directory_path()
           / ("engine-fuzz-sceneservice-" + std::to_string(fuzz::currentPid()));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root / ".cache" / "meshs", ec);
    fs::create_directories(g_root / ".cache" / "scenes", ec);

    for (const char* name : { "a.cooked", "b.cooked" }) {
        if (writeRealMesh(g_root / ".cache" / "meshs" / name))
            g_realMeshes.push_back(std::string("meshs/") + name);
    }
    if (g_realMeshes.empty()) {
        std::printf("FAIL could not write the fixture meshes\n");
        return 1;
    }

    const int rc = fuzz::run("scene_service", argc, argv, oneCase);

    fs::remove_all(g_root, ec);
    bgfx::shutdown();
    return rc;
}
