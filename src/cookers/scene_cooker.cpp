#include "cookers/scene_cooker.h"
#include <assetlib/scene_asset.h>
#include <assetlib/asset_registry.h>
#include "engine/logger.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <string>

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
            auto [off, len] = assetlib::stringTableAppend(cooked.stringTable, name);
            ent.nameOffset = off;
            ent.nameLength = len;
        }

        // MeshRenderer
        if (je.contains("meshRenderer")) {
            ent.componentMask |= assetlib::kComp_MeshRenderer;
            const auto& mr = je["meshRenderer"];
            std::string srcPath    = mr.value("sourcePath", std::string{});
            std::string srcType    = mr.value("sourceType", std::string{});
            std::string cookedPath = mr.value("cookedPath", std::string{});

            // If no cookedPath in the JSON, try to resolve it via the asset DB
            if (cookedPath.empty() && !srcPath.empty() && assetLib
                    && !projectRoot.empty()
                    && srcPath.rfind("engine://", 0) != 0) {
                std::error_code ec;
                auto rel = std::filesystem::relative(srcPath, projectRoot, ec);
                if (!ec && !rel.empty()) {
                    auto rec = assetLib->findBySourcePath(rel.generic_string());
                    if (rec && rec->state == assetlib::AssetState::Ready
                            && !rec->cookedPath.empty())
                        cookedPath = rec->cookedPath;
                }
            }

            auto [coff, clen] = assetlib::stringTableAppend(cooked.stringTable, cookedPath);
            ent.meshCookedOffset = coff;
            ent.meshCookedLength = clen;
            auto [soff, slen] = assetlib::stringTableAppend(cooked.stringTable, srcPath);
            ent.meshSourceOffset = soff;
            ent.meshSourceLength = slen;
            ent.meshSourceType   = (srcType == "primitive") ? 1 : 0;
            ent.matOverrideId    = mr.value("matOverrideId", 0u);
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
            auto [off, len] = assetlib::stringTableAppend(cooked.stringTable, path);
            ent.scriptPathOffset = off;
            ent.scriptPathLength = len;
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
