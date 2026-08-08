#include "assets/cookers/scene/scene_cooker.h"
#include <assetlib/scene_asset.h>
#include <assetlib/asset_registry.h>
#include "core/logger.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

// Resolve a meshRenderer JSON record to its asset-DB record: UUID first
// (survives renames), then project-relative source path. Shared by the cook
// pass and the per-scene dependency staleness check.
static std::optional<assetlib::AssetRecord> resolveMeshRecord(
        const nlohmann::json& mr,
        assetlib::AssetRegistry* assetLib,
        const std::filesystem::path& projectRoot) {
    if (!assetLib) return std::nullopt;
    std::string srcUuid = mr.value("asset", std::string{});
    std::string srcPath = mr.value("path", std::string{});
    if (srcPath.empty())
        srcPath = mr.value("sourcePath", std::string{});
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
    for (const auto& je : scene.value("entities", nlohmann::json::array())) {
        if (!je.contains("meshRenderer")) continue;
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
        const std::string declared = j.value(
            "name", std::filesystem::path(rec.sourcePath).stem().string());
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
            for (const auto& je : scene.value("entities", nlohmann::json::array())) {
                if (!je.contains("meshRenderer")) continue;
                const auto& mr = je["meshRenderer"];
                if (auto rec = resolveMeshRecord(mr, assetLib, projectRoot))
                    out.insert(rec->uuid.toString());

                // Materials are referenced by AUTHORED NAME, not by path, so
                // they cannot be resolved the way meshes are. Without this a
                // scene-scoped cook produced no .cmat at all and the material
                // silently did not exist at runtime — authoring one and running
                // the game gave you the mesh's baked material with no
                // indication anything was missing.
                const std::string matName = mr.value("material", std::string{});
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
                   const std::filesystem::path& projectRoot) {
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

    assetlib::SceneAsset cooked;
    const auto entities = scene.value("entities", nlohmann::json::array());

    // Deduplicating string interner. stringTableAppend is pure append —
    // without this, N entities sharing one mesh path stored the path N
    // times (a 10k-prop scene bloated its table by ~megabytes of repeats;
    // cooker audit's "string table" finding — the O(N²) claim there was
    // wrong, the real defect was duplicate bloat). O(1) per lookup.
    std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> interned;
    auto intern = [&](const std::string& s) -> std::pair<uint32_t, uint32_t> {
        if (s.empty()) return {0xFFFFFFFF, 0};
        auto it = interned.find(s);
        if (it != interned.end()) return it->second;
        auto entry = assetlib::stringTableAppend(cooked.stringTable, s);
        interned.emplace(s, entry);
        return entry;
    };

    for (const auto& je : entities) {
        assetlib::SceneEntity ent{};
        ent.entityId = je.value("id", (uint64_t)0);
        ent.parentId = je.value("parentId", (uint64_t)0);

        // Transform
        if (je.contains("transform")) {
            ent.componentMask |= assetlib::kComp_Transform;
            const auto& t = je["transform"];
            if (t.contains("position")) {
                ent.position[0] = t["position"][0];
                ent.position[1] = t["position"][1];
                ent.position[2] = t["position"][2];
            }
            if (t.contains("rotation")) {
                ent.rotation[0] = t["rotation"][0];
                ent.rotation[1] = t["rotation"][1];
                ent.rotation[2] = t["rotation"][2];
                ent.rotation[3] = t["rotation"][3];
            }
            if (t.contains("scale")) {
                ent.scale[0] = t["scale"][0];
                ent.scale[1] = t["scale"][1];
                ent.scale[2] = t["scale"][2];
            }
        }

        // Name
        if (je.contains("name")) {
            ent.componentMask |= assetlib::kComp_Name;
            std::string name = je["name"].is_string()
                ? je["name"].get<std::string>() : "Entity";
            auto [off, len] = intern(name);
            ent.nameOffset = off;
            ent.nameLength = len;
        }

        // MeshRenderer
        if (je.contains("meshRenderer")) {
            ent.componentMask |= assetlib::kComp_MeshRenderer;
            const auto& mr = je["meshRenderer"];
            // New format: "asset" (uuid) + "path" (project-relative).
            // Legacy: "sourcePath" (absolute). The cooked binary stores an
            // absolute-or-relative source path string for the runtime loader.
            std::string srcPath    = mr.value("path", std::string{});
            if (srcPath.empty())
                srcPath = mr.value("sourcePath", std::string{});
            std::string srcType    = mr.value("sourceType", std::string{});
            std::string cookedPath = mr.value("cookedPath", std::string{});

            // Resolve cookedPath via the asset DB (shared resolver — same
            // logic drives the dependency staleness check).
            //
            // The DB WINS over any cookedPath cached in the scene JSON. That
            // field is a stale snapshot of a previous registry generation: a
            // teammate who clones the repo, anyone whose registry.db is
            // regenerated, or a machine that cooks fresh will mint different
            // UUIDs for the same files, and the JSON's remembered
            // "meshs/<old-uuid>.cooked" then points at a file that does not
            // exist. Trusting it produced a scene that cooked "successfully"
            // and loaded NOTHING at runtime — every mesh reported
            // "Cannot stat" and the level came up empty.
            //
            // AssetRef's contract is uuid-first, path-fallback precisely so
            // identity survives this; the resolver honours it, so ask it first
            // and keep the JSON value only as a last resort.
            if (auto rec = resolveMeshRecord(mr, assetLib, projectRoot);
                rec && rec->state == assetlib::AssetState::Ready
                    && !rec->cookedPath.empty()) {
                if (!cookedPath.empty() && cookedPath != rec->cookedPath)
                    LOG_INFO("SceneCook", "Healed stale cooked ref: %s -> %s",
                             cookedPath.c_str(), rec->cookedPath.c_str());
                cookedPath = rec->cookedPath;
                // Keep the runtime's source fallback current too.
                if (!rec->sourcePath.empty()) srcPath = rec->sourcePath;
            }

            auto [coff, clen] = intern(cookedPath);
            ent.meshCookedOffset = coff;
            ent.meshCookedLength = clen;
            auto [soff, slen] = intern(srcPath);
            ent.meshSourceOffset = soff;
            ent.meshSourceLength = slen;
            ent.meshSourceType   = (srcType == "primitive") ? 1 : 0;
            // The material by AUTHORED NAME. Interned like every other string;
            // null-terminated, so no length field is needed (SceneEntity has no
            // spare bytes — see materialNameOffset).
            const std::string matName = mr.value("material", std::string{});
            if (!matName.empty()) {
                auto [moff, mlen] = intern(matName);
                (void)mlen;
                ent.materialNameOffset = moff;
            }
        }

        // Camera
        if (je.contains("camera")) {
            ent.componentMask |= assetlib::kComp_Camera;
            const auto& c = je["camera"];
            ent.cameraIsPrimary  = c.value("isPrimary", true) ? 1 : 0;
            ent.cameraProjection = static_cast<uint8_t>(c.value("projection", 0));
            ent.cameraFov        = c.value("fov", 60.0f);
            ent.cameraOrthoSize  = c.value("orthoSize", 10.0f);
            ent.cameraNearPlane  = c.value("nearPlane", 0.1f);
            ent.cameraFarPlane   = c.value("farPlane", 1000.0f);
            if (c.contains("clearColor")) {
                const auto& cc = c["clearColor"];
                ent.cameraClearColor[0] = cc[0]; ent.cameraClearColor[1] = cc[1];
                ent.cameraClearColor[2] = cc[2]; ent.cameraClearColor[3] = cc[3];
            }
        }

        // RigidBody
        if (je.contains("rigidBody")) {
            ent.componentMask |= assetlib::kComp_RigidBody;
            const auto& rb = je["rigidBody"];
            ent.rbBodyType    = static_cast<uint8_t>(rb.value("bodyType", 1));
            ent.rbShape       = static_cast<uint8_t>(rb.value("shape", 0));
            ent.rbMass        = rb.value("mass", 1.0f);
            ent.rbRestitution = rb.value("restitution", 0.3f);
            ent.rbFriction    = rb.value("friction", 0.6f);
            ent.rbUseGravity  = rb.value("useGravity", true) ? 1 : 0;
            if (rb.contains("halfExtent")) {
                const auto& he = rb["halfExtent"];
                ent.rbHalfExtent[0] = he[0]; ent.rbHalfExtent[1] = he[1]; ent.rbHalfExtent[2] = he[2];
            }
            ent.rbRadius     = rb.value("radius", 0.5f);
            ent.rbHalfHeight = rb.value("halfHeight", 0.5f);
        }

        // Script
        if (je.contains("script")) {
            ent.componentMask |= assetlib::kComp_Script;
            std::string path = je["script"].value("path", std::string{});
            auto [off, len] = intern(path);
            ent.scriptPathOffset = off;
            ent.scriptPathLength = len;
        }

        // Animator (v2) — skeletal clip selection must survive the cook or
        // shipped skinned meshes T-pose (clip binding is runtime-resolved
        // from clipPath / embedded clipIndex; handles never serialize).
        if (je.contains("animator")) {
            ent.componentMask |= assetlib::kComp_Animator;
            const auto& a = je["animator"];
            ent.animClipIndex = (int16_t)a.value("clipIndex", 0);
            ent.animSpeed     = a.value("speed", 1.0f);
            ent.animFade      = a.value("fade", 0.2f);
            ent.animPlaying   = a.value("playing", false) ? 1 : 0;
            ent.animLooping   = a.value("looping", true) ? 1 : 0;
            std::string clipPath = a.value("path", std::string{});
            auto [off, len] = intern(clipPath);
            ent.animClipPathOffset = off;
            ent.animClipPathLength = len;
        }

        // CharacterController
        if (je.contains("characterController")) {
            ent.componentMask |= assetlib::kComp_CharacterController;
            const auto& cc = je["characterController"];
            ent.ccRadius       = cc.value("radius", 0.3f);
            ent.ccHeight       = cc.value("height", 1.8f);
            ent.ccMaxSlopeDeg  = cc.value("maxSlopeDeg", 45.0f);
            ent.ccStepHeight   = cc.value("stepHeight", 0.3f);
            ent.ccMass         = cc.value("mass", 70.0f);
            ent.ccGravityScale = cc.value("gravityScale", 1.0f);
        }

        // Light
        if (je.contains("light")) {
            ent.componentMask |= assetlib::kComp_Light;
            const auto& l = je["light"];
            ent.lightType        = static_cast<uint8_t>(l.value("type", 0));
            if (l.contains("color")) {
                const auto& c = l["color"];
                ent.lightColor[0] = c[0]; ent.lightColor[1] = c[1]; ent.lightColor[2] = c[2];
            }
            ent.lightIntensity    = l.value("intensity", 3.0f);
            ent.lightRange        = l.value("range", 15.0f);
            ent.lightSpotInner    = l.value("spotInner", 25.0f);
            ent.lightSpotOuter    = l.value("spotOuter", 35.0f);
            ent.lightCastShadows  = l.value("castShadows", false) ? 1 : 0;
            ent.lightUseTemp      = l.value("useTemperature", false) ? 1 : 0;
            ent.lightTemperatureK = l.value("temperatureK", 6500.0f);
        }

        cooked.entities.push_back(ent);
    }

    std::filesystem::create_directories(outPath.parent_path());

    if (!assetlib::saveScene(cooked, outPath)) {
        LOG_ERROR("SceneCook", "Failed to write binary: %s", outPath.string().c_str());
        return false;
    }

    LOG_SUCCESS("SceneCook", "Cooked %zu entities → %s (strings: %zu B)",
                cooked.entities.size(), outPath.filename().string().c_str(),
                cooked.stringTable.size());
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

    for (const auto& je : scene.value("entities", nlohmann::json::array())) {
        if (!je.contains("meshRenderer")) continue;
        auto rec = resolveMeshRecord(je["meshRenderer"], assetLib, projectRoot);
        if (!rec || rec->cookedPath.empty()) continue;

        std::error_code ec;
        auto depTime = std::filesystem::last_write_time(
            cacheRoot / rec->cookedPath, ec);
        if (!ec && depTime > cookedTime) return true;   // THIS scene's dep moved
    }
    return false;
}
