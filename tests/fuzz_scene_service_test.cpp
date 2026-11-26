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
#include <nlohmann/json.hpp>
#include "assets/cooked_scene.h"
#include "scene/entity_serializer.h"
#include <assetlib/mesh_asset.h>

#include "gpu_test_device.h"
#include <flecs.h>

#include <cstring>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// 2: cooked scene format 4 (WO-091); 3: half packed, payload corrupted (WO-092).
// The corpus's seeds were found under 1.
static constexpr uint32_t kGeneratorVersion = 3;

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
// hands over untouched and only the consumer interprets. Since WO-091 (format 4)
// the payload is the scene's own form, so the components are the registry's
// keys, with values both well-formed and not.
nlohmann::json junk(fuzz::Rng& rng, int depth = 0) {
    switch (depth > 2 ? rng.below(4) : rng.below(6)) {
        case 0:  return nullptr;
        case 1:  return rng.chance(50);
        case 2:  return (int64_t)rng.interestingU32();
        case 3: {
            std::string v;
            for (uint32_t k = rng.range(0, 40); k > 0; --k) v.push_back((char)rng.range(32, 126));
            return v;
        }
        case 4: {
            nlohmann::json a = nlohmann::json::array();
            for (uint32_t k = rng.range(0, 4); k > 0; --k) a.push_back(junk(rng, depth + 1));
            return a;
        }
        default: {
            nlohmann::json o = nlohmann::json::object();
            for (uint32_t k = rng.range(0, 4); k > 0; --k) o["k" + std::to_string(k)] = junk(rng, depth + 1);
            return o;
        }
    }
}

nlohmann::json buildScene(fuzz::Rng& rng) {
    nlohmann::json scene = { {"version", 1}, {"entities", nlohmann::json::array()} };
    const uint32_t n = rng.range(0, 40);
    std::vector<uint64_t> issuedIds;

    for (uint32_t i = 0; i < n; ++i) {
        nlohmann::json e = nlohmann::json::object();
        // Identity: duplicates and zero are the interesting cases. A cooked
        // scene with two entities claiming one id must resolve, not alias.
        uint64_t id;
        switch (rng.below(5)) {
            case 0:  id = 0; break;                                    // "no id"
            case 1:  id = issuedIds.empty() ? rng.next()               // duplicate
                        : issuedIds[rng.below((uint32_t)issuedIds.size())]; break;
            case 2:  id = 1; break;                                    // collides with anything
            default: id = rng.next(); break;
        }
        issuedIds.push_back(id);
        if (id || rng.chance(50)) e["id"] = id;

        // Parenting: this is the adversarial part. safeReparent is the only
        // thing standing between these and an infinite ancestor walk.
        uint64_t parent = 0;
        switch (rng.below(6)) {
            case 0:  parent = 0; break;                                // root
            case 1:  parent = id; break;                               // SELF-parent
            case 2:  parent = issuedIds.empty() ? 0                    // a real ancestor
                            : issuedIds[rng.below((uint32_t)issuedIds.size())]; break;
            case 3:  parent = rng.next(); break;                       // dangling
            // A deliberate chain: each entity parented to the PREVIOUS one, so
            // a 40-entity scene builds a 40-deep hierarchy. Combined with the
            // self-parent and duplicate-id cases above, this is how a cycle
            // gets built out of individually reasonable records.
            default: parent = i ? issuedIds[i - 1] : 0; break;
        }
        if (parent) e["parentId"] = rng.chance(10) ? junk(rng) : nlohmann::json(parent);

        e["transform"] = { {"position", {(double)((int32_t)rng.next() % 1000), 0.0, 0.0}},
                           {"scale", {rng.chance(10) ? 0.0 : 1.0, 1.0, 1.0}} };
        if (rng.chance(70)) e["name"] = rng.chance(10) ? junk(rng) : nlohmann::json("e" + std::to_string(i));
        if (rng.chance(65)) {
            // Half the time a mesh that really loads — otherwise the GPU and
            // residency paths would never run and this would be a parse test
            // with extra steps.
            nlohmann::json mr = nlohmann::json::object();
            if (rng.chance(50) && !g_realMeshes.empty())
                mr["cookedPath"] = g_realMeshes[rng.below((uint32_t)g_realMeshes.size())];
            else
                mr["cookedPath"] = junk(rng);
            if (rng.chance(40)) mr["path"] = junk(rng);
            if (rng.chance(40)) mr["material"] = junk(rng);
            if (rng.chance(20)) mr["sourceType"] = "primitive";
            e["meshRenderer"] = rng.chance(10) ? junk(rng) : mr;
        }
        // The engine's components, each as junk or with junk inside, and the
        // shapes only the registry reads: a kit's block, an unknown key, a
        // revision from the future, an LOD chain naming meshes.
        static const char* kKeys[] = { "camera", "rigidBody", "script", "characterController",
                                       "light", "animator", "skinnedMesh", "colourGrading",
                                       "lodMesh", "reflected", "sprite" };
        for (const char* k : kKeys)
            if (rng.chance(15)) e[k] = junk(rng);
        if (rng.chance(10)) e["camera"] = { {"$rev", rng.interestingU32()}, {"fov", 10} };
        if (rng.chance(10))
            e["animator"] = { {"clipIndex", (int64_t)(int32_t)rng.next()}, {"path", junk(rng)},
                              {"playing", rng.chance(50)} };
        if (rng.chance(8) && !g_realMeshes.empty())
            e["lodMesh"] = { {"levels", { { {"cookedPath", g_realMeshes[0]} }, junk(rng) }},
                             {"coarsenBelow", junk(rng)} };

        scene["entities"].push_back(rng.chance(3) ? junk(rng) : e);   // sometimes not an entity
    }
    return scene;
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
        assetlib::SceneAsset scene;
        // Half the cases packed, as the tools cook (WO-092), half as CBOR.
        const nlohmann::json js = buildScene(rng);
        std::string why;
        if (!rng.chance(50) || !EntitySerde::packScene(js, scene, why))
            cooked_scene::encode(js, scene);
        // Corrupt the PAYLOAD with the header kept consistent, so the damage
        // reaches the payload readers rather than the container's size check:
        // flipped bits, or a payload cut short with its size rewritten.
        if (!scene.payload.empty() && rng.chance(20))
            for (uint32_t k = rng.range(1, 8); k > 0; --k)
                scene.payload[rng.below((uint32_t)scene.payload.size())] ^= (uint8_t)(1u << rng.below(8));
        if (!scene.payload.empty() && rng.chance(10))
            scene.payload.resize(rng.below((uint32_t)scene.payload.size()));
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
    if (!initTestDevice()) return 1;

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
    shutdownTestDevice();
    return rc;
}
