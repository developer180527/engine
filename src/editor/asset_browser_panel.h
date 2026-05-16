#include "engine/logger.h"
#include "engine/async_loader.h"
#pragma once

#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>

#include <imgui.h>
#include <flecs.h>

#include "engine_context.h"
#include "io/asset_storage.h"
#include "core/transform.h"
#include "components/name.h"
#include "components/mesh_renderer.h"
#include "components/spinner.h"
#include "render/mesh.h"

// Asset browser panel.
//
// Shows the contents of ctx.project.assetsRoot, filtered to files that
// have a registered importer. The user can:
//   - See all loadable files with size and load status
//   - Single-click to select and preview file info
//   - Double-click (or press Load button) to load and spawn into the scene
//   - Change the assets folder via the path field
//   - Reload the file list via the Refresh button
//
// Loading is cached: loading the same file twice returns the cached mesh.
// Spawned entities appear in the hierarchy immediately (the hierarchy
// panel iterates ECS, so it reflects the new entity on the next frame).
// The spawned entity is auto-selected so the gizmo appears on it.
//
// Auto-scale: if the loaded mesh has valid AABB data, we scale the entity
// so its largest dimension is approximately 2 world units — matching the
// cube grid's unit size. The user can then rescale via inspector or gizmo.

namespace asset_browser_detail {

struct FileEntry {
    std::string         name;      // filename only (e.g. "Duck.gltf")
    std::string         fullPath;  // absolute path
    uintmax_t           sizeBytes = 0;
    bool                supported = false;
    bool                loaded    = false;
};

// Format a byte count as a human-readable string.
inline std::string formatSize(uintmax_t bytes) {
    char buf[32];
    if      (bytes >= 1024*1024*1024)
        std::snprintf(buf, sizeof(buf), "%.1f GB", bytes / (1024.0*1024*1024));
    else if (bytes >= 1024*1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0*1024));
    else if (bytes >= 1024)
        std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%llu B",
                      static_cast<unsigned long long>(bytes));
    return buf;
}

// Scan a directory and populate a sorted file list.
inline std::vector<FileEntry> scanDirectory(
    const std::filesystem::path& dir,
    const ImporterRegistry& importers)
{
    namespace fs = std::filesystem;
    std::vector<FileEntry> entries;

    if (!fs::is_directory(dir)) return entries;

    for (const auto& de : fs::directory_iterator(dir)) {
        if (!de.is_regular_file()) continue;

        FileEntry e;
        e.name      = de.path().filename().string();
        e.fullPath  = de.path().string();
        e.supported = importers.supportsFile(de.path());
        try { e.sizeBytes = de.file_size(); } catch (...) {}
        entries.push_back(std::move(e));
    }

    // Supported files first, then alphabetical within each group.
    std::sort(entries.begin(), entries.end(),
        [](const FileEntry& a, const FileEntry& b) {
            if (a.supported != b.supported) return a.supported > b.supported;
            return a.name < b.name;
        });

    return entries;
}

// Derive a display name from a filename: strip the extension.
inline std::string baseName(const std::string& filename) {
    auto dot = filename.find_last_of('.');
    return dot != std::string::npos ? filename.substr(0, dot) : filename;
}

// Compute a sensible uniform scale so the mesh fits within ~2 world units.
// Uses the mesh AABB if available; falls back to scale=1 otherwise.
inline float autoScale(const Mesh* mesh) {
    if (!mesh || !mesh->hasBounds()) return 1.0f;
    const bx::Vec3 size = mesh->boundsSize();
    const float largest = std::max({size.x, size.y, size.z});
    if (largest < 1e-4f) return 1.0f;

    // Target: largest dimension = 2 units (matches our cube grid size).
    const float target = 2.0f;
    return target / largest;
}

// Compute the Y offset needed so the mesh sits on Y=0 after scaling.
// Without this, models might spawn halfway underground or floating.
inline float groundOffset(const Mesh* mesh, float scale) {
    if (!mesh || !mesh->hasBounds()) return 0.0f;
    // The mesh's lowest point in object space, scaled to world space.
    return -mesh->boundsMin.y * scale;
}

} // namespace asset_browser_detail

inline void drawAssetBrowserPanel(EngineContext& ctx, AsyncLoader& loader) {
    namespace fs = std::filesystem;
    using namespace asset_browser_detail;

    // Persistent panel state — lives across frames.
    static std::vector<FileEntry> s_files;
    static int  s_selectedIdx  = -1;
    static bool s_needsRefresh = true;
    static char s_pathBuf[512] = {};

    // Sync path buffer with project context on first open or after changes.
    if (s_needsRefresh || s_pathBuf[0] == '\0') {
        std::string pathStr = ctx.project.assetsRoot.string();
        std::snprintf(s_pathBuf, sizeof(s_pathBuf), "%s", pathStr.c_str());
    }

    ImGui::Begin("Assets");

    // ---- Path field + Refresh button ----
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f);
    if (ImGui::InputText("##path", s_pathBuf, sizeof(s_pathBuf),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        ctx.project.assetsRoot = std::filesystem::path(s_pathBuf);
        s_needsRefresh = true;
        s_selectedIdx  = -1;
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        s_needsRefresh = true;
        s_selectedIdx  = -1;
    }

    ImGui::Separator();

    // ---- Refresh file list if needed ----
    if (s_needsRefresh) {
        s_files = scanDirectory(ctx.project.assetsRoot, ctx.importers);
        // Update loaded status from registry cache.
        for (auto& f : s_files)
            f.loaded = ctx.importers.isLoaded(f.fullPath)
                     || loader.isLoaded(f.fullPath);
        s_needsRefresh = false;
    } else {
        // Update loaded flags without full rescan.
        for (auto& f : s_files)
            f.loaded = ctx.importers.isLoaded(f.fullPath)
                     || loader.isLoaded(f.fullPath);
    }

    // ---- File list ----
    if (s_files.empty()) {
        ImGui::TextDisabled("(no supported files in folder)");
    }

    ImGui::BeginChild("##filelist",
                      ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 36),
                      false);

    for (int i = 0; i < static_cast<int>(s_files.size()); ++i) {
        const FileEntry& f = s_files[i];

        // Dim unsupported files so the user can see what's there but
        // understands they can't load it yet.
        if (!f.supported)
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));

        // Loaded checkmark prefix.
        std::string label = f.loaded ? "  [+] " : "      ";
        label += f.name;
        label += "  " + formatSize(f.sizeBytes);

        const bool isSelected = (s_selectedIdx == i);
        ImGui::PushID(i);

        if (ImGui::Selectable(label.c_str(), isSelected,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            s_selectedIdx = i;

            // Double-click on a supported file: load and spawn.
            if (ImGui::IsMouseDoubleClicked(0) && f.supported) {
                // Route by extension: glTF/GLB → cgltf (sync), others → Assimp (async)
                {
                    std::string _ext = std::filesystem::path(f.fullPath).extension().string();
                    for (auto& _c : _ext) _c = (char)std::tolower(_c);
                    const bool _isGltf = (_ext == ".glb" || _ext == ".gltf");
                    if (_isGltf) {
                        AssetStorage _s{ctx.assets, ctx.textures, ctx.materials};
                        auto _r = ctx.importers.loadCached(f.fullPath, _s);
                        if (_r.success) {
                            if (Mesh* _m = const_cast<Mesh*>(ctx.assets.getMesh(_r.mesh)))
                                _m->sourcePath = f.fullPath;
                            const Mesh* _mesh = ctx.assets.getMesh(_r.mesh);
                            const float _sc = autoScale(_mesh);
                            const float _yo = groundOffset(_mesh, _sc);
                            Transform _t; _t.position={0,_yo,0}; _t.scale={_sc,_sc,_sc};
                            ctx.editor.selected = ctx.ecs.entity(baseName(f.name).c_str())
                                .set<Transform>(_t)
                                .set<MeshRenderer>({_r.mesh})
                                .set<Name>({baseName(f.name)});
                            LOG_SUCCESS("Loader", "Spawned '%s' (glTF sync)", baseName(f.name).c_str());
                        } else {
                            LOG_ERROR("Loader", "glTF load failed: %s", _r.error.c_str());
                        }
                    } else {
                        auto& _ecs = ctx.ecs; auto& _assets = ctx.assets; auto& _ed = ctx.editor;
                        std::string _en = baseName(f.name);
                        LOG_INFO("Loader", "Queued: %s", f.name.c_str());
                        loader.load(f.fullPath, _en,
                            [&_ecs, &_assets, &_ed, _en](MeshHandle h, const std::string&) {
                                if (!h.valid()) return;
                                const Mesh* mesh = _assets.getMesh(h);
                                const float sc = autoScale(mesh);
                                const float yo = groundOffset(mesh, sc);
                                Transform t; t.position={0,yo,0}; t.scale={sc,sc,sc};
                                _ed.selected = _ecs.entity(_en.c_str())
                                    .set<Transform>(t)
                                    .set<MeshRenderer>({h})
                                    .set<Name>({_en});
                                LOG_SUCCESS("Loader", "Spawned '%s'", _en.c_str());
                            });
                    }
                }
            }
        }

        if (!f.supported) ImGui::PopStyleColor();
        ImGui::PopID();
    }

    ImGui::EndChild();

    // ---- Bottom action bar ----
    ImGui::Separator();

    const bool canLoad = (s_selectedIdx >= 0
                          && s_selectedIdx < (int)s_files.size()
                          && s_files[s_selectedIdx].supported);

    // Show async loading indicator
    if (canLoad && loader.isLoading(s_files[s_selectedIdx].fullPath)) {
        ImGui::TextDisabled("Loading... (%.0f in queue)",
                           (float)loader.pendingCount());
    }
    if (!canLoad) ImGui::BeginDisabled();
    if (ImGui::Button("Load & Spawn") && canLoad) {
        const FileEntry& f = s_files[s_selectedIdx];
        // Route by extension: glTF/GLB → cgltf (sync), others → Assimp (async)
        {
            std::string _ext = std::filesystem::path(f.fullPath).extension().string();
            for (auto& _c : _ext) _c = (char)std::tolower(_c);
            const bool _isGltf = (_ext == ".glb" || _ext == ".gltf");
            if (_isGltf) {
                AssetStorage _s{ctx.assets, ctx.textures, ctx.materials};
                auto _r = ctx.importers.loadCached(f.fullPath, _s);
                if (_r.success) {
                    if (Mesh* _m = const_cast<Mesh*>(ctx.assets.getMesh(_r.mesh)))
                        _m->sourcePath = f.fullPath;
                    const Mesh* _mesh = ctx.assets.getMesh(_r.mesh);
                    const float _sc = autoScale(_mesh);
                    const float _yo = groundOffset(_mesh, _sc);
                    Transform _t; _t.position={0,_yo,0}; _t.scale={_sc,_sc,_sc};
                    ctx.editor.selected = ctx.ecs.entity(baseName(f.name).c_str())
                        .set<Transform>(_t)
                        .set<MeshRenderer>({_r.mesh})
                        .set<Name>({baseName(f.name)});
                    LOG_SUCCESS("Loader", "Spawned '%s' (glTF sync)", baseName(f.name).c_str());
                } else {
                    LOG_ERROR("Loader", "glTF load failed: %s", _r.error.c_str());
                }
            } else {
                auto& _ecs = ctx.ecs; auto& _assets = ctx.assets; auto& _ed = ctx.editor;
                std::string _en = baseName(f.name);
                LOG_INFO("Loader", "Queued: %s", f.name.c_str());
                loader.load(f.fullPath, _en,
                    [&_ecs, &_assets, &_ed, _en](MeshHandle h, const std::string&) {
                        if (!h.valid()) return;
                        const Mesh* mesh = _assets.getMesh(h);
                        const float sc = autoScale(mesh);
                        const float yo = groundOffset(mesh, sc);
                        Transform t; t.position={0,yo,0}; t.scale={sc,sc,sc};
                        _ed.selected = _ecs.entity(_en.c_str())
                            .set<Transform>(t)
                            .set<MeshRenderer>({h})
                            .set<Name>({_en});
                        LOG_SUCCESS("Loader", "Spawned '%s'", _en.c_str());
                    });
            }
        }
    }
    if (!canLoad) ImGui::EndDisabled();

    ImGui::SameLine();

    if (ImGui::Button("Clear Scene")) {
        // Delete all entities that have a MeshRenderer but are NOT cubes
        // (cubes have the Spinner component; glTF models don't by default).
        std::vector<flecs::entity> toDelete;
        ctx.ecs.query_builder<MeshRenderer>()
            .build()
            .each([&](flecs::entity e, MeshRenderer&) {
                if (!e.has<Spinner>()) toDelete.push_back(e);
            });
        for (auto e : toDelete) e.destruct();
        ctx.editor.selected = flecs::entity{};
    }

    ImGui::End();
}
