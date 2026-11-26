// ── mesh_ownership_test — unloading a SHARED mesh ───────────────────────────
//
// BUG-0052, and it is BUG-0051 one resource type over. `unloadMesh` ended in:
//
//     return m_meshes.removeMesh(h);
//
// unconditionally — for a resource `m_loadedMeshes` dedups by path and, until
// this fix, did not refcount. `loadMesh`'s cache-hit path hands every
// subsequent caller `it->second.h`, so N loads of one .cmesh give N callers the
// SAME handle, and the first unload destroyed it for all of them.
//
// Silent, for the same reason the texture version was: `AssetRegistry::removeMesh`
// pushes the slot onto a free list with no generation counter, so the next
// `addMesh` pops it and the survivor reads a live `Mesh` that is simply the
// wrong one. Wrong geometry, no crash, nothing in the log. Reachable from Lua
// as `assets.unloadMesh`.
//
// It sat about twenty lines above the `unloadTexture` that BUG-0051 fixed, in
// the same file, in the same week.
//
// ── WHAT THIS TEST REFUSES TO DO ────────────────────────────────────────────
// BUG-0049 was a regression test that compared a constant to itself and would
// have passed with the defect fully restored. So every assertion here reads
// state the PRODUCTION code owns: handles from the `AssetRegistry` the service
// writes, mesh pointers from `getMesh` the renderer dereferences, and refcounts
// from the same `m_texCache` the service acquires into. Nothing is satisfiable
// by the test alone.
#include <cstdio>
#include <filesystem>

#include <assetlib/asset_registry.h>
#include <assetlib/mesh_asset.h>
#include <assetlib/texture_asset.h>

#include "gpu_test_device.h"
#include "render/asset_registry.h"
#include "render/material_registry.h"
#include "render/texture_registry.h"
#include "render/vertex.h"
#include "runtime/services/asset_service.h"
#include "render/skinned_vertex.h"
#include "animation/clip_registry.h"
#include "animation/ozz_bridge.h"
#include "animation/skeleton_registry.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>

namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)


// A real SKINNED cooked mesh: two bones, an ozz skeleton archive and one
// embedded clip, through the real writer. What a skinned character is on disk.
static std::vector<uint8_t> drainStream(ozz::io::MemoryStream& ms) {
    const int n = ms.Tell();
    std::vector<uint8_t> out((size_t)n);
    ms.Seek(0, ozz::io::Stream::kSet);
    ms.Read(out.data(), (size_t)n);
    return out;
}
static bool writeSkinnedMesh(const fs::path& out) {
    Skeleton s;
    s.bones.resize(2);
    s.bones[0].name = "root"; s.bones[0].parentIndex = -1;
    s.bones[1].name = "arm";  s.bones[1].parentIndex = 0;
    s.buildBoneMap();
    anim::buildOzzSkeleton(s);
    if (!s.ozz) return false;
    assetlib::MeshAsset m;
    m.header.version      = 6;
    m.header.vertexFlags  = assetlib::VF_POSITION | assetlib::VF_NORMAL | assetlib::VF_UV0
                          | assetlib::VF_TANGENT | assetlib::VF_JOINTS | assetlib::VF_WEIGHTS;
    m.header.vertexStride = sizeof(SkinnedVertex);
    m.header.vertexCount  = 3;
    m.header.indexStride  = 4;
    m.header.indexCount   = 3;
    m.header.boneCount    = 2;
    m.vertexData.assign((size_t)m.header.vertexCount * m.header.vertexStride, 0);
    const uint32_t idx[3] = { 0, 1, 2 };
    m.indexData.assign((const uint8_t*)idx, (const uint8_t*)idx + sizeof(idx));
    for (int i = 0; i < 3; ++i) { m.header.boundsMin[i] = -1.0f; m.header.boundsMax[i] = 1.0f; }
    for (const Bone& b : s.bones) {
        assetlib::CookedBone cb{};
        std::snprintf(cb.name, sizeof(cb.name), "%s", b.name.c_str());
        cb.parentIndex = b.parentIndex;
        cb.bindRotation[3] = 1.0f;
        cb.bindScale[0] = cb.bindScale[1] = cb.bindScale[2] = 1.0f;
        for (int k = 0; k < 16; ++k)
            cb.inverseBindMatrix[k] = cb.localBindMatrix[k] = (k % 5 == 0) ? 1.0f : 0.0f;
        m.bones.push_back(cb);
    }
    { ozz::io::MemoryStream ms; { ozz::io::OArchive a(&ms); a << *s.ozz; } m.skeletonBlob = drainStream(ms); }
    m.skeletonBlobDigest = assetlib::blobDigest(m.skeletonBlob);
    ozz::animation::offline::RawAnimation raw;
    raw.duration = 1.0f;
    raw.tracks.resize((size_t)s.ozz->num_joints());
    ozz::animation::offline::AnimationBuilder builder;
    auto built = builder(raw);
    if (!built) return false;
    assetlib::CookedClipBlob clip;
    clip.name = "idle"; clip.mappedTracks = clip.totalTracks = 2;
    { ozz::io::MemoryStream ms; { ozz::io::OArchive a(&ms); a << *built; } clip.blob = drainStream(ms); }
    clip.blobDigest = assetlib::blobDigest(clip.blob);
    m.clips.push_back(std::move(clip));
    return assetlib::saveMesh(m, out);
}

static bool writeCtex(const fs::path& out) {
    assetlib::TextureAsset t;
    t.header.width    = 2;
    t.header.height   = 2;
    t.header.format   = assetlib::kTexRGBA8;
    t.header.mipCount = 1;
    t.pixels.assign(2 * 2 * 4, 0xFF);
    return assetlib::saveTexture(t, out);
}

// A cooked mesh with one embedded material naming `texRel`. The geometry is a
// single degenerate triangle: this fixture exists to be loaded and unloaded,
// not drawn.
static bool writeCookedMesh(const fs::path& out, const char* texRel) {
    assetlib::MeshAsset m;
    m.header.vertexStride  = sizeof(Vertex);
    m.header.vertexCount   = 3;
    m.header.indexCount    = 3;
    m.header.indexStride   = 4;
    m.header.submeshCount  = 1;
    m.header.materialCount = 1;
    m.vertexData.assign(3 * sizeof(Vertex), 0);
    const uint32_t idx[3] = {0, 1, 2};
    m.indexData.assign(reinterpret_cast<const uint8_t*>(idx),
                       reinterpret_cast<const uint8_t*>(idx) + sizeof idx);

    assetlib::MeshSubmesh sub;
    sub.indexOffset = 0; sub.indexCount = 3; sub.materialIndex = 0;
    m.submeshes.push_back(sub);

    assetlib::CookedMaterial cm;
    cm.flags = assetlib::kMatFlag_HasBaseColor;
    std::snprintf(cm.baseColorPath, sizeof cm.baseColorPath, "%s", texRel);
    m.materials.push_back(cm);

    return assetlib::saveMesh(m, out);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("mesh_ownership_test: unloading a shared mesh\n");

    if (!initTestDevice()) return 1;

    const fs::path root = fs::temp_directory_path() / "engine_mesh_ownership_test";
    fs::remove_all(root);
    fs::create_directories(root / ".cache");
    const fs::path ctex  = root / ".cache" / "shared.ctex";
    const fs::path cmesh = root / ".cache" / "shared.cmesh";
    if (!writeCtex(ctex) || !writeCookedMesh(cmesh, "shared.ctex")) {
        std::printf("  FAIL  could not author the fixtures\n");
        return 1;
    }

    AssetRegistry            meshes;
    TextureRegistry          textures;
    MaterialRegistry         materials;
    assetlib::AssetRegistry  assetLib;

    AssetService::Config cfg{ meshes, textures, materials, &assetLib, root };
    AssetService svc(cfg);

    const char* kRel = "shared.cmesh";
    const std::string texKey = ctex.lexically_normal().generic_string();

    // ── 1. Two loads of one path share ONE mesh ────────────────────────────
    MeshHandle a{}, b{};
    {
        std::printf("\n-- 1. two callers, one mesh --\n");
        a = svc.loadMesh(kRel);
        b = svc.loadMesh(kRel);
        CHECK(a.valid() && a.id == b.id,
              "the same path yields the same handle (%u, %u) — dedup, not two "
              "uploads", a.id, b.id);
        CHECK(meshes.getMesh(a) != nullptr, "the registry holds it");
        // The texture reference is taken ONCE, by the load that missed. This is
        // what makes the release in unloadMesh balance: one acquire, one release.
        CHECK(svc.textureCache().refCount(texKey) == 1,
              "its material's texture is acquired once, by the miss (%u)",
              svc.textureCache().refCount(texKey));
    }

    // ── 2. THE REGRESSION: one unload must not destroy it ───────────────────
    // Before the fix this reached `m_meshes.removeMesh(h)` unconditionally, so
    // getMesh(b) returned null here — and worse, the slot went on the free list
    // and the next addMesh handed it to an unrelated mesh.
    {
        std::printf("\n-- 2. one unload, with a holder remaining --\n");
        CHECK(svc.unloadMesh(a),
              "the first unload reports success — the caller DID give back its "
              "reference; the resource simply outlives it");
        CHECK(meshes.getMesh(b) != nullptr,
              "and the mesh is STILL RESIDENT for the second holder — this is "
              "the assertion the unconditional removeMesh failed");
        CHECK(svc.textureCache().refCount(texKey) == 1,
              "its texture is still referenced too (%u) — releasing it here "
              "would make it evictable while a live mesh's material points at it",
              svc.textureCache().refCount(texKey));
    }

    // ── 3. A reload is still a cache HIT, not a resurrection ───────────────
    // If the entry had been erased on the first unload, this would re-read the
    // file and produce a DIFFERENT handle — the dedup silently broken.
    {
        std::printf("\n-- 3. the cache entry survived --\n");
        const MeshHandle c = svc.loadMesh(kRel);
        // HONEST LABEL: this section is NOT part of the regression proof, and
        // saying so is cheaper than letting a future reader assume it is.
        // Measured against the mutation: with the refs gate removed this whole
        // section still passes, because the destroyed slot goes on the free
        // list and the reload pops the same id back with a freshly loaded mesh
        // in it. Same number, live pointer, different object — which is exactly
        // the silence that makes the bug expensive, and exactly why no
        // assertion available here can see it.
        //
        // Sections 2 and 4 are the discriminating ones. This documents that
        // dedup survives a partial unload, which is worth pinning on its own.
        CHECK(c.id == b.id && meshes.getMesh(c) != nullptr,
              "reloading returns the same live handle (%u == %u) — dedup was "
              "not broken by the partial unload", c.id, b.id);
        CHECK(svc.unloadMesh(c), "and that third reference gives back cleanly");
    }

    // ── 4. The LAST unload destroys, and gives the textures back ───────────
    {
        std::printf("\n-- 4. the last holder leaves --\n");
        CHECK(svc.unloadMesh(b), "the final unload succeeds");
        CHECK(meshes.getMesh(b) == nullptr,
              "the mesh is gone — refs reached 0, so this one really did destroy");
        CHECK(svc.textureCache().refCount(texKey) == 0,
              "and the texture reference the load took is given back (%u). "
              "Without this the texture budget reclaims nothing on the normal "
              "path, because the cache correctly refuses to evict a referenced "
              "entry", svc.textureCache().refCount(texKey));
        // refs == 0 is EVICTABLE, not dead — the same contract the texture
        // cache states and texture_ownership_test pins.
        CHECK(textures.getTexture(
                  TextureHandle{ (uint32_t)svc.queryTexture("shared.ctex") }) != nullptr
              || svc.textureCache().contains(texKey),
              "the texture itself is retained until eviction, not freed on the "
              "last release");
    }

    // ── 5. Unloading a handle nothing loaded is a clean false ──────────────
    {
        std::printf("\n-- 5. an unknown handle --\n");
        CHECK(!svc.unloadMesh(b),
              "unloading an already-destroyed handle returns false, not true — "
              "`true` from the shared path means 'your reference was released', "
              "and this handle has none left to give");
    }

    // ── 6. A load that ASKS FOR THE SKIN, of a static mesh (WO-091) ────────
    // `loadMesh` used to return a cached handle only when the caller wanted no
    // skin; a caller passing `outSkin` fell through to a full load and
    // superseded the entry, because the entry could not hand back a skin. A
    // scene load always asks (it cannot know which meshes are skinned), so 50 000
    // entities over one static mesh read its file 50 000 times. The entry now
    // records whether the mesh is static, and a skin-asking hit on it is an
    // ordinary shared hit with an empty skin: one mesh, one texture reference,
    // released on the last unload.
    //
    // The supersede path the old §6 and §7 covered (re-key the entry rather than
    // overwrite it, BUG-0052) is unchanged code, now reached only by an entry
    // whose skin is unknown: an async load, or a skinned mesh whose skeleton
    // blob did not decode.
    {
        std::printf("\n-- 6. a skin-asking load of a static mesh shares it --\n");
        AssetService::MeshSkin skinA{}, skinB{};
        skinA.skeleton.id = 99;   // must come back emptied
        const MeshHandle sa = svc.loadMesh(kRel, &skinA);
        const MeshHandle sb = svc.loadMesh(kRel, &skinB);
        CHECK(sa.valid() && sa.id == sb.id,
              "two skin-asking loads share one mesh (%u, %u), as loads without do",
              sa.id, sb.id);
        CHECK(!skinA.skeleton.valid() && !skinB.skeleton.valid() && skinB.clips.empty(),
              "and each is told it has no skin");
        CHECK(svc.textureCache().refCount(texKey) == 1,
              "its material's texture is acquired once (%u)",
              svc.textureCache().refCount(texKey));
        CHECK(svc.unloadMesh(sa) && meshes.getMesh(sb) != nullptr,
              "the first unload leaves the mesh resident for the second holder");
        CHECK(svc.unloadMesh(sb) && meshes.getMesh(sb) == nullptr,
              "the second destroys it, once");
        CHECK(svc.textureCache().refCount(texKey) == 0,
              "leaving nothing referenced (%u)", svc.textureCache().refCount(texKey));
    }

    // ── 7. Mixed holders: with and without the skin ────────────────────────
    {
        std::printf("\n-- 7. holders with and without the skin share one count --\n");
        const MeshHandle p = svc.loadMesh(kRel);
        const MeshHandle q = svc.loadMesh(kRel);
        AssetService::MeshSkin skin{};
        const MeshHandle r = svc.loadMesh(kRel, &skin);
        CHECK(p.valid() && p.id == q.id && q.id == r.id,
              "three holders, one handle (%u)", p.id);
        CHECK(svc.unloadMesh(p) && svc.unloadMesh(q) && meshes.getMesh(r) != nullptr,
              "two leave, and it is still resident for the third");
        CHECK(svc.unloadMesh(r) && meshes.getMesh(r) == nullptr, "the third destroys it, once");
        CHECK(svc.textureCache().refCount(texKey) == 0,
              "with every texture reference given back (%u)",
              svc.textureCache().refCount(texKey));
    }

    // ── 8. A SKINNED mesh shared by two entities (WO-091) ──────────────────
    // With the skeleton and clip registries wired, as every runtime has them, a
    // second skin-asking load of the same skinned mesh hits the cache. Every
    // caller must get the skin: an entity whose load returns no skeleton is
    // drawn in bind pose and never animates.
    {
        std::printf("\n-- 8. two loads of one skinned mesh, each with its skin --\n");
        SkeletonRegistry skeletons;
        AnimClipRegistry clips;
        AssetService::Config scfg{ meshes, textures, materials, &assetLib, root };
        scfg.skeletons = &skeletons; scfg.clips = &clips;
        AssetService skinned(scfg);
        CHECK(writeSkinnedMesh(root / ".cache" / "hero.cmesh"), "a skinned cooked mesh is written");
        AssetService::MeshSkin first{}, second{};
        const MeshHandle h1 = skinned.loadMesh("hero.cmesh", &first);
        const MeshHandle h2 = skinned.loadMesh("hero.cmesh", &second);
        CHECK(h1.valid() && first.skeleton.valid() && first.clips.size() == 1,
              "the first load has its skeleton and clip (skeleton %u, %zu clip(s))",
              first.skeleton.id, first.clips.size());
        CHECK(h2.valid() && second.skeleton.valid() && second.clips.size() == 1,
              "the second load has them too (skeleton %u, %zu clip(s))",
              second.skeleton.id, second.clips.size());
        if (h2.valid()) skinned.unloadMesh(h2);
        if (h1.valid()) skinned.unloadMesh(h1);
    }

    fs::remove_all(root);

    if (g_failures) {
        std::printf("\nmesh_ownership_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nmesh_ownership_test: ALL PASS\n");
    return 0;
}
