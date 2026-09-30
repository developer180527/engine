// ── scene_mesh_reference_test — loading never deletes an authored mesh (WO-029) ─
//
// The defect: an entity whose mesh the loader could not resolve got no
// MeshRenderer, the save path writes only what a MeshRenderer holds, so the NEXT
// SAVE wrote the entity without its mesh — and the reference was gone for good.
// fps_shooter lost three models that way, silently.
//
// Like reflected_pending_test, every case goes THROUGH a session that cannot
// resolve the mesh and saves from it, because the save is the only step that
// destroys anything. Hermetic: a temp project root with no assets in it is the
// whole fixture — no GPU, no importer, no registry.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "scene/entity_serializer.h"
#include "scene/unresolved_mesh.h"
#include "runtime/sim_classification.h"
#include "components/sim_state.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

namespace fs = std::filesystem;
using namespace EntitySerde;
using nlohmann::json;

// Exactly what fps_shooter's house looked like before its reference was lost:
// uuid + project-relative path + a cooked path + a material override. Every
// field must come back, not just the path.
static json houseRef() {
    return json::parse(R"({
        "asset": "6f1c2a9e-0b7d-4c1e-9a55-3e2f8d1b7c40",
        "path": "assets/models/house.fbx",
        "cookedPath": "meshs/6f1c2a9e-0b7d-4c1e-9a55-3e2f8d1b7c40.cooked",
        "material": "Brick"
    })");
}
static json entityWith(const json& mesh) {
    json je;
    je["name"] = "house";
    je["transform"] = {{"position", {1.0, 2.0, 3.0}}, {"rotation", {0, 0, 0, 1}},
                       {"scale", {1, 1, 1}}};
    je["meshRenderer"] = mesh;
    return je;
}
static SerdeContext diskCtx(const fs::path& root, std::vector<PendingMesh>* pending) {
    SerdeContext c;
    c.mode = SerdeMode::Disk;
    c.projectRoot = root;
    c.pendingAsync = pending;
    return c;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("scene_mesh_reference_test\n");

    const fs::path root = fs::temp_directory_path() / "wo029_scene_mesh_reference";
    fs::remove_all(root);
    fs::create_directories(root / "assets/models");

    // ── 1. Missing source: the reference survives a save, field for field ──
    {
        std::printf("1. source file missing\n");
        flecs::world w;
        std::vector<PendingMesh> pending;
        SerdeContext ctx = diskCtx(root, &pending);
        flecs::entity e = createEntity(w, entityWith(houseRef()), ctx, IdPolicy::Generate);

        CHECK(!e.has<MeshRenderer>(), "no MeshRenderer (nothing could load)");
        const UnresolvedMesh* u = e.try_get<UnresolvedMesh>();
        CHECK(u != nullptr, "the authored reference is kept on the entity");
        CHECK(u && u->reason == "source not found", "reason: %s", u ? u->reason.c_str() : "-");
        CHECK(u && !u->pending, "not pending — nothing is in flight");
        CHECK(pending.empty(), "no async load was queued for a missing file");
        CHECK(u && unresolved_mesh::describe(*u) == "assets/models/house.fbx",
              "describe() names the authored path");

        const json saved = saveEntity(e, ctx);
        CHECK(saved.contains("meshRenderer"), "the save still writes a meshRenderer");
        CHECK(saved.value("meshRenderer", json{}) == houseRef(),
              "…and it is the authored reference, unchanged: %s",
              saved.value("meshRenderer", json{}).dump().c_str());
    }

    // ── 2. Async import still in flight when the scene is saved ────────────
    {
        std::printf("2. async (Assimp) load not finished at save time\n");
        { std::ofstream(root / "assets/models/house.fbx") << "not really an fbx"; }
        flecs::world w;
        std::vector<PendingMesh> pending;
        SerdeContext ctx = diskCtx(root, &pending);
        flecs::entity e = createEntity(w, entityWith(houseRef()), ctx, IdPolicy::Generate);

        const UnresolvedMesh* u = e.try_get<UnresolvedMesh>();
        CHECK(pending.size() == 1, "one async load queued");
        CHECK(u && u->pending, "marked pending, not failed");
        CHECK(saveEntity(e, ctx).value("meshRenderer", json{}) == houseRef(),
              "saving mid-load writes the authored reference unchanged");

        // The build has no importer at all (a server, a shipping player).
        SerdeContext noImport = diskCtx(root, nullptr);
        flecs::entity e2 = createEntity(w, entityWith(houseRef()), noImport, IdPolicy::Generate);
        const UnresolvedMesh* u2 = e2.try_get<UnresolvedMesh>();
        CHECK(u2 && !u2->pending && u2->reason.find("no importer") != std::string::npos,
              "no importer: kept, with a reason (%s)", u2 ? u2->reason.c_str() : "-");
        CHECK(saveEntity(e2, noImport).value("meshRenderer", json{}) == houseRef(),
              "…and saved back unchanged");
        fs::remove(root / "assets/models/house.fbx");
    }

    // ── 3. Through a memory snapshot (undo, Snapshot Play) and back to disk ─
    {
        std::printf("3. memory snapshot round trip\n");
        flecs::world edit;
        SerdeContext disk = diskCtx(root, nullptr);
        flecs::entity e = createEntity(edit, entityWith(houseRef()), disk, IdPolicy::Generate);

        SerdeContext mem; mem.mode = SerdeMode::Memory;
        const json snap = saveEntity(e, mem);
        CHECK(snap["meshRenderer"].contains("unresolved"), "the snapshot carries it");

        flecs::world restored;
        flecs::entity r = createEntity(restored, snap, mem, IdPolicy::Preserve);
        CHECK(r.has<UnresolvedMesh>(), "restoring the snapshot puts it back");
        CHECK(r.get<UnresolvedMesh>().reason == "source not found", "with its reason");
        CHECK(saveEntity(r, disk).value("meshRenderer", json{}) == houseRef(),
              "and a disk save after the restore still writes the authored reference");
    }

    // ── 4. A resolved mesh wins over a stale reference ─────────────────────
    {
        std::printf("4. a MeshRenderer wins\n");
        flecs::world w;
        SerdeContext ctx = diskCtx(root, nullptr);
        flecs::entity e = createEntity(w, entityWith(houseRef()), ctx, IdPolicy::Generate);
        Mesh cube; cube.sourcePath = "engine://primitive/cube";
        ctx.meshLookup = [&cube](MeshHandle) -> const Mesh* { return &cube; };
        MeshHandle h; h.id = 7;
        e.set<MeshRenderer>({h});
        const json m = saveEntity(e, ctx).value("meshRenderer", json{});
        CHECK(m.value("path", std::string{}) == "engine://primitive/cube",
              "the live mesh is saved, not the old reference: %s", m.dump().c_str());
    }

    // ── 5. Nothing authored: nothing kept, and the file is unchanged ───────
    {
        std::printf("5. empty meshRenderer object\n");
        flecs::world w;
        SerdeContext ctx = diskCtx(root, nullptr);
        flecs::entity e = createEntity(w, entityWith(json::object()), ctx, IdPolicy::Generate);
        CHECK(!e.has<UnresolvedMesh>(), "no reference was authored, so none is kept");
        CHECK(!saveEntity(e, ctx).contains("meshRenderer"),
              "and the save omits the key, as it always did");
    }

    // ── 6. A malformed reference is kept too — it is still the user's data ─
    {
        std::printf("6. malformed meshRenderer\n");
        flecs::world w;
        SerdeContext ctx = diskCtx(root, nullptr);
        const json odd = json::parse(R"({"path": "assets/models/house.fbx", "cookedPath": 42})");
        flecs::entity e = createEntity(w, entityWith(odd), ctx, IdPolicy::Generate);
        // `cookedPath: 42` makes nlohmann's value() THROW inside loadMesh, which
        // createEntity's tolerant() catches — the load stops part-way.
        CHECK(e.has<UnresolvedMesh>(), "a load that threw part-way still keeps the reference");
        CHECK(saveEntity(e, ctx).value("meshRenderer", json{}) == odd,
              "and writes it back byte-for-byte in meaning");
    }

    // ── 7. Classified for the determinism gate ─────────────────────────────
    {
        std::printf("7. classification\n");
        flecs::world w;
        simhash::registerClassification(w);
        CHECK(w.component<UnresolvedMesh>().has<SimExempt>(),
              "UnresolvedMesh is SimExempt (auditCoverage would fail on it otherwise)");
    }

    fs::remove_all(root);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
