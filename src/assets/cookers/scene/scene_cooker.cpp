#include "assets/cookers/scene/scene_cooker.h"
#include <assetlib/scene_asset.h>
#include <assetlib/asset_registry.h>
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
    const auto entities = entitiesOf(scene);

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
        if (!je.is_object()) continue;      // an array of numbers is not entities
        assetlib::SceneEntity ent{};
        // jsonread::u64, not j.value(): `value()` THROWS on a key that exists
        // with the wrong type, and the only try/catch here wraps the parse — so
        // `"id": "3"` escaped this function, on CookService's background thread.
        ent.entityId = jsonread::readU64(je, "id", 0);
        ent.parentId = jsonread::readU64(je, "parentId", 0);

        // Transform
        if (je.contains("transform")) {
            ent.componentMask |= assetlib::kComp_Transform;
            const auto& t = je["transform"];
            // ── The fifth copy of the same UB ───────────────────────────────
            // nlohmann's CONST operator[](size_type) is unchecked: `"position":
            // []` indexed straight into an empty vector. core/json_read.h swept
            // four of these (entity_serializer, editor_prefs, undo_stack) — and
            // missed the cooker, which reads the SAME .scene file a human edits.
            // readFloats bounds-checks, type-checks, rejects non-finite, and
            // leaves the destination alone otherwise, so a short array keeps the
            // identity default instead of a partly-zeroed transform.
            jsonread::readFloats(t, "position", ent.position, 3);
            jsonread::readFloats(t, "rotation", ent.rotation, 4);
            jsonread::readFloats(t, "scale",    ent.scale,    3);
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
            std::string srcPath    = jsonread::readString(mr, "path");
            if (srcPath.empty())
                srcPath = jsonread::readString(mr, "sourcePath");
            std::string srcType    = jsonread::readString(mr, "sourceType");
            std::string cookedPath = jsonread::readString(mr, "cookedPath");

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
            const std::string matName = jsonread::readString(mr, "material");
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
            ent.cameraIsPrimary  = jsonread::readBool(c, "isPrimary", true) ? 1 : 0;
            ent.cameraProjection = static_cast<uint8_t>(jsonread::readU32(c, "projection", 0));
            ent.cameraFov        = jsonread::readFloat(c, "fov", 60.0f);
            ent.cameraOrthoSize  = jsonread::readFloat(c, "orthoSize", 10.0f);
            ent.cameraNearPlane  = jsonread::readFloat(c, "nearPlane", 0.1f);
            ent.cameraFarPlane   = jsonread::readFloat(c, "farPlane", 1000.0f);
            jsonread::readFloats(c, "clearColor", ent.cameraClearColor, 4);
        }

        // RigidBody
        if (je.contains("rigidBody")) {
            ent.componentMask |= assetlib::kComp_RigidBody;
            const auto& rb = je["rigidBody"];
            ent.rbBodyType    = static_cast<uint8_t>(jsonread::readU32(rb, "bodyType", 1));
            ent.rbShape       = static_cast<uint8_t>(jsonread::readU32(rb, "shape", 0));
            ent.rbMass        = jsonread::readFloat(rb, "mass", 1.0f);
            ent.rbRestitution = jsonread::readFloat(rb, "restitution", 0.3f);
            ent.rbFriction    = jsonread::readFloat(rb, "friction", 0.6f);
            ent.rbUseGravity  = jsonread::readBool(rb, "useGravity", true) ? 1 : 0;
            jsonread::readFloats(rb, "halfExtent", ent.rbHalfExtent, 3);
            ent.rbRadius     = jsonread::readFloat(rb, "radius", 0.5f);
            ent.rbHalfHeight = jsonread::readFloat(rb, "halfHeight", 0.5f);
        }

        // Script
        if (je.contains("script")) {
            ent.componentMask |= assetlib::kComp_Script;
            std::string path = jsonread::readString(je["script"], "path");
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
            ent.animClipIndex = (int16_t)jsonread::readU32(a, "clipIndex", 0);
            ent.animSpeed     = jsonread::readFloat(a, "speed", 1.0f);
            ent.animFade      = jsonread::readFloat(a, "fade", 0.2f);
            ent.animPlaying   = jsonread::readBool(a, "playing", false) ? 1 : 0;
            ent.animLooping   = jsonread::readBool(a, "looping", true) ? 1 : 0;
            std::string clipPath = jsonread::readString(a, "path");
            auto [off, len] = intern(clipPath);
            ent.animClipPathOffset = off;
            ent.animClipPathLength = len;
        }

        // CharacterController
        if (je.contains("characterController")) {
            ent.componentMask |= assetlib::kComp_CharacterController;
            const auto& cc = je["characterController"];
            ent.ccRadius       = jsonread::readFloat(cc, "radius", 0.3f);
            ent.ccHeight       = jsonread::readFloat(cc, "height", 1.8f);
            ent.ccMaxSlopeDeg  = jsonread::readFloat(cc, "maxSlopeDeg", 45.0f);
            ent.ccStepHeight   = jsonread::readFloat(cc, "stepHeight", 0.3f);
            ent.ccMass         = jsonread::readFloat(cc, "mass", 70.0f);
            ent.ccGravityScale = jsonread::readFloat(cc, "gravityScale", 1.0f);
        }

        // Light
        if (je.contains("light")) {
            ent.componentMask |= assetlib::kComp_Light;
            const auto& l = je["light"];
            ent.lightType        = static_cast<uint8_t>(jsonread::readU32(l, "type", 0));
            jsonread::readFloats(l, "color", ent.lightColor, 3);
            ent.lightIntensity    = jsonread::readFloat(l, "intensity", 3.0f);
            ent.lightRange        = jsonread::readFloat(l, "range", 15.0f);
            ent.lightSpotInner    = jsonread::readFloat(l, "spotInner", 25.0f);
            ent.lightSpotOuter    = jsonread::readFloat(l, "spotOuter", 35.0f);
            ent.lightCastShadows  = jsonread::readBool(l, "castShadows", false) ? 1 : 0;
            ent.lightUseTemp      = jsonread::readBool(l, "useTemperature", false) ? 1 : 0;
            ent.lightTemperatureK = jsonread::readFloat(l, "temperatureK", 6500.0f);
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
