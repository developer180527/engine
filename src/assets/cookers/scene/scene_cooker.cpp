#include "assets/cookers/scene/scene_cooker.h"
#include <assetlib/scene_asset.h>
#include <assetlib/asset_registry.h>
#include "assets/cooked_scene.h"
#include "core/logger.h"
#include "core/json_read.h"   // every read out of a hand-editable .scene

#include <nlohmann/json.hpp>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

// `scene.value("entities", array())` THROWS when the key exists with a
// non-array type, and every caller below reads it — so a scene whose "entities"
// is a number took the cooker down instead of being reported. Returns an empty
// array for anything that is not one, which then cooks to an empty scene: an
// honest, inspectable result for a file that declares no usable entities.
static const nlohmann::json& entitiesOf(const nlohmann::json& scene) {
    static const nlohmann::json kEmpty = nlohmann::json::array();
    if (!scene.is_object()) return kEmpty;
    const auto it = scene.find("entities");
    return (it != scene.end() && it->is_array()) ? *it : kEmpty;
}

// Resolve a meshRenderer JSON record to its asset-DB record: UUID first
// (survives renames), then project-relative source path. Shared by the cook
// pass and the per-scene dependency staleness check.
static std::optional<assetlib::AssetRecord> resolveMeshRecord(
        const nlohmann::json& mr,
        assetlib::AssetRegistry* assetLib,
        const std::filesystem::path& projectRoot) {
    if (!assetLib) return std::nullopt;
    std::string srcUuid = jsonread::readString(mr, "asset");
    std::string srcPath = jsonread::readString(mr, "path");
    if (srcPath.empty())
        srcPath = jsonread::readString(mr, "sourcePath");
    if (srcPath.rfind("engine://", 0) == 0) return std::nullopt;

    std::optional<assetlib::AssetRecord> rec;
    if (!srcUuid.empty())
        rec = assetLib->findByUUID(assetlib::UUID::fromString(srcUuid));
    if (!rec && !srcPath.empty() && !projectRoot.empty()) {
        std::string rel = srcPath;
        if (std::filesystem::path(srcPath).is_absolute()) {
            std::error_code ec;
            auto r = std::filesystem::relative(srcPath, projectRoot, ec);
            if (ec || r.empty()) rel.clear();
            else rel = r.generic_string();
        }
        if (!rel.empty())
            rec = assetLib->findBySourcePath(rel);
    }
    return rec;
}

std::unordered_set<std::string> collectSceneRefs(
        const std::filesystem::path& jsonPath,
        assetlib::AssetRegistry* assetLib,
        const std::filesystem::path& projectRoot) {
    std::unordered_set<std::string> out;
    if (!assetLib) return out;
    std::ifstream f(jsonPath);
    if (!f) return out;
    nlohmann::json scene;
    try { scene = nlohmann::json::parse(f); }
    catch (...) { return out; }         // broken scene — no refs, no edges
    for (const auto& je : entitiesOf(scene)) {
        if (!je.is_object() || !je.contains("meshRenderer")) continue;
        if (auto rec = resolveMeshRecord(je["meshRenderer"], assetLib, projectRoot))
            out.insert(rec->uuid.toString());
    }
    return out;
}

// name -> the .material asset declaring it. The name lives INSIDE the file
// (falling back to the stem), so this reads each registered .material once.
// Cheap by construction: materials are few, and the alternative — assuming
// name == filename — silently breaks the moment an author sets "name".
static std::optional<assetlib::AssetRecord> resolveMaterialByName(
        const std::string& name, assetlib::AssetRegistry* assetLib,
        const std::filesystem::path& projectRoot) {
    for (const auto& rec : assetLib->all()) {
        if (std::filesystem::path(rec.sourcePath).extension() != ".material")
            continue;
        std::ifstream f(projectRoot / rec.sourcePath);
        if (!f) continue;
        nlohmann::json j;
        try { j = nlohmann::json::parse(f); } catch (...) { continue; }
        const std::string declared = jsonread::readString(
            j, "name", std::filesystem::path(rec.sourcePath).stem().string());
        if (declared == name) return rec;
    }
    return std::nullopt;
}

std::unordered_set<std::string> collectSceneAssetClosure(
        const std::vector<std::filesystem::path>& sceneDirs,
        assetlib::AssetRegistry* assetLib,
        const std::filesystem::path& projectRoot) {
    std::unordered_set<std::string> out;
    if (!assetLib) return out;
    std::error_code ec;
    for (const auto& dir : sceneDirs) {
        if (!std::filesystem::is_directory(dir, ec)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            if (entry.path().extension() != ".scene") continue;
            std::ifstream f(entry.path());
            if (!f) continue;
            nlohmann::json scene;
            try { scene = nlohmann::json::parse(f); }
            catch (...) { continue; }   // a broken scene mustn't abort the walk
            for (const auto& je : entitiesOf(scene)) {
                if (!je.is_object() || !je.contains("meshRenderer")) continue;
                const auto& mr = je["meshRenderer"];
                if (auto rec = resolveMeshRecord(mr, assetLib, projectRoot))
                    out.insert(rec->uuid.toString());

                // Materials are referenced by AUTHORED NAME, not by path, so
                // they cannot be resolved the way meshes are. Without this a
                // scene-scoped cook produced no .cmat at all and the material
                // silently did not exist at runtime — authoring one and running
                // the game gave you the mesh's baked material with no
                // indication anything was missing.
                const std::string matName = jsonread::readString(mr, "material");
                if (!matName.empty())
                    if (auto rec = resolveMaterialByName(matName, assetLib, projectRoot))
                        out.insert(rec->uuid.toString());
            }
        }
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// cookSceneFile — JSON scene → binary scene
//
// This is the standalone version of SceneSerializer::cookScene() with minimal
// include footprint. CookService calls this on its background thread; EditorApp
// calls it on the main thread after saving.
// ═══════════════════════════════════════════════════════════════════════════

bool cookSceneFile(const std::filesystem::path& jsonPath,
                   const std::filesystem::path& outPath,
                   assetlib::AssetRegistry* assetLib,
                   const std::filesystem::path& projectRoot,
                   const cooked_scene::Packer* packer) {
    if (!std::filesystem::exists(jsonPath)) {
        LOG_ERROR("SceneCook", "JSON scene not found: %s", jsonPath.string().c_str());
        return false;
    }

    nlohmann::json scene;
    try {
        std::ifstream f(jsonPath);
        scene = nlohmann::json::parse(f);
    } catch (const std::exception& e) {
        LOG_ERROR("SceneCook", "Parse error in %s: %s",
                  jsonPath.filename().string().c_str(), e.what());
        return false;
    }

    // ── The payload is the scene itself (WO-091) ────────────────────────────
    // What the editor saved, every component kept, so the shipped player reads
    // it through the same schema registry and nothing is lost in the cook. The
    // only change is to mesh references, healed against the asset database.
    //
    // The DB WINS over any cookedPath cached in the scene JSON. That field is a
    // stale snapshot of a previous registry generation: a teammate who clones the
    // repo, anyone whose registry.db is regenerated, or a machine that cooks
    // fresh will mint different UUIDs for the same files, and the JSON's
    // remembered "meshs/<old-uuid>.cooked" then points at a file that does not
    // exist. Trusting it produced a scene that cooked "successfully" and loaded
    // NOTHING at runtime. AssetRef's contract is uuid-first, path-fallback
    // precisely so identity survives this; the resolver honours it, so ask it
    // first and keep the JSON value only as a last resort.
    auto heal = [&](nlohmann::json& ref) {
        if (!ref.is_object()) return;
        auto rec = resolveMeshRecord(ref, assetLib, projectRoot);
        if (!rec || rec->state != assetlib::AssetState::Ready || rec->cookedPath.empty())
            return;
        const std::string was = jsonread::readString(ref, "cookedPath");
        if (!was.empty() && was != rec->cookedPath)
            LOG_INFO("SceneCook", "Healed stale cooked ref: %s -> %s",
                     was.c_str(), rec->cookedPath.c_str());
        ref["cookedPath"] = rec->cookedPath;
    };
    size_t count = 0;
    if (scene.is_object() && scene.contains("entities") && scene["entities"].is_array()) {
        for (auto& je : scene["entities"]) {
            if (!je.is_object()) continue;
            ++count;
            if (auto mr = je.find("meshRenderer"); mr != je.end()) heal(*mr);
            if (auto lm = je.find("lodMesh"); lm != je.end() && lm->is_object())
                if (auto lv = lm->find("levels"); lv != lm->end() && lv->is_array())
                    for (auto& level : *lv) heal(level);
        }
    } else {
        // Not a scene with entities: cooked as an empty one, which is the honest
        // result for a file that declares none (see entitiesOf).
        scene = { {"version", 1}, {"entities", nlohmann::json::array()} };
    }

    assetlib::SceneAsset cooked;
    const char* encoding = "CBOR";
    std::string packWhy;
    if (packer && *packer && (*packer)(scene, cooked, packWhy)) {
        encoding = "packed";
    } else {
        if (packer && *packer)
            LOG_WARN("SceneCook", "%s: not packed (%s) — written as CBOR, which loads slower",
                     jsonPath.filename().string().c_str(), packWhy.c_str());
        cooked_scene::encode(scene, cooked);
    }
    std::filesystem::create_directories(outPath.parent_path());
    if (!assetlib::saveScene(cooked, outPath)) {
        LOG_ERROR("SceneCook", "Failed to write binary: %s", outPath.string().c_str());
        return false;
    }
    LOG_SUCCESS("SceneCook", "Cooked %zu entities → %s (%zu B, %s)", count,
                outPath.filename().string().c_str(), cooked.payload.size(), encoding);
    return true;
}

bool sceneDependsOnNewerAssets(const std::filesystem::path& jsonPath,
                               std::filesystem::file_time_type cookedTime,
                               assetlib::AssetRegistry* assetLib,
                               const std::filesystem::path& projectRoot,
                               const std::filesystem::path& cacheRoot) {
    if (!assetLib) return false;

    nlohmann::json scene;
    try {
        std::ifstream f(jsonPath);
        scene = nlohmann::json::parse(f);
    } catch (const std::exception&) {
        return false;   // broken scene: surfaced by the cook itself, not here
    }

    for (const auto& je : entitiesOf(scene)) {
        if (!je.is_object() || !je.contains("meshRenderer")) continue;
        auto rec = resolveMeshRecord(je["meshRenderer"], assetLib, projectRoot);
        if (!rec || rec->cookedPath.empty()) continue;

        std::error_code ec;
        auto depTime = std::filesystem::last_write_time(
            cacheRoot / rec->cookedPath, ec);
        if (!ec && depTime > cookedTime) return true;   // THIS scene's dep moved
    }
    return false;
}
