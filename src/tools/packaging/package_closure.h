#pragma once
// ── Packaging closure — which files must a dist contain? ────────────────────
//
// ONE concern: given a cooked mesh, name every file the runtime will look for.
//
// This lived inline in engine_build's main flow, which is why it was wrong for
// months without anything noticing. The bug it now encodes a test for:
//
//   engine_build copied a mesh's sibling .ctex by GUESSING "<meshUuid>_t*.ctex"
//   from the mesh's own filename. That holds only while a mesh and its textures
//   are cooked together in one pass. It is false the moment the DDC
//   materializes a cache — the manifest restores siblings under the names they
//   were FIRST written with, while the mesh output takes the CURRENT uuid.
//
//   A clean rebuild against a warm shared DDC therefore shipped a game with no
//   textures at all. Silently: every mesh present, the game runs, every surface
//   untextured. That is an ordinary CI flow, not an exotic one.
//
// The fix, and the rule this header exists to hold: **ship what the cooked
// asset REFERENCES, never what its filename suggests.** A reference cannot
// drift from what the runtime resolves, because it is the same string.
#include <filesystem>
#include <assetlib/asset_registry.h>
#include <string>
#include <vector>

namespace pkg {

struct MeshClosure {
    // Absolute paths that must be copied, mesh first. Deduplicated: two meshes
    // sharing one texture (the cooker's content-dedup makes that normal) must
    // not produce two copy operations for the same file.
    std::vector<std::filesystem::path> files;
    // Textures the mesh references that are NOT on disk. Never silently
    // dropped: an absent texture means the shipped game renders untextured, so
    // the caller reports these and the build is visibly suspect.
    std::vector<std::string> missing;
    // The cooked mesh could not be read at all.
    bool unreadable = false;
};

// `cookedMesh` is an absolute path to a <uuid>.cooked mesh. Texture references
// are stored as basenames and resolved against the mesh's own directory —
// exactly how AssetService resolves them at runtime.
MeshClosure meshClosure(const std::filesystem::path& cookedMesh);

// ── Which meshes ship at all ────────────────────────────────────────────────
struct SceneRefs {
    // Cache-relative mesh paths ("meshs/<uuid>.cooked"), deduplicated and
    // ordered, since a mesh referenced by two scenes must ship once.
    std::vector<std::string> meshes;
    // Scenes that would not load. The old inline version `continue`d past
    // these, so a single corrupt or version-bumped scene dropped EVERY object
    // it contained from the package with no output at all — a strictly worse
    // failure than the untextured one, and equally silent.
    std::vector<std::string> unreadableScenes;
    // Scenes read successfully. Zero, with a directory that exists, means the
    // package has no content: worth saying out loud.
    size_t scenesRead = 0;
};

// Walks <cache>/scenes/*.cooked and collects every mesh the entities reference.
SceneRefs sceneMeshRefs(const std::filesystem::path& scenesDir);

// ── Cooked shaders ──────────────────────────────────────────────────────────
// Counting files is not enough here. The runtime resolves shaders BY THE NAME
// each .cshader declares inside itself (a dist has no registry), and asks for
// "standard" unconditionally. A package can contain shader files and still
// leave the game on compiled-in fallbacks if none of them declares the name
// being asked for — which renders correctly, so nothing looks wrong, and the
// project's custom shading simply never runs.
struct ShaderSet {
    std::vector<std::filesystem::path> files;    // sorted, ship all of them
    std::vector<std::string> names;              // declared names, sorted
    // Files that will not load. They would be shipped as dead weight and the
    // shader they were meant to provide would be missing.
    std::vector<std::string> unreadable;
    // One name declared by two files. ShaderLibrary picks deterministically by
    // filename, so one silently shadows the other — normally the fingerprint of
    // a stale cook left behind by an older cooker version.
    std::vector<std::string> duplicateNames;

    bool provides(const std::string& name) const;
};

ShaderSet shaderFiles(const std::filesystem::path& shadersDir);

// ── Cooked materials ────────────────────────────────────────────────────────
// Same shape as shaders, and for the same reason: a game asks for a material by
// its authored NAME, so a package can hold material files and still not provide
// the one the game names. Materials are not reachable from scenes yet, so the
// whole directory ships — a .cmat is a few hundred bytes.
struct MaterialSet {
    std::vector<std::filesystem::path> files;   // sorted
    std::vector<std::string> names;             // declared names, sorted
    std::vector<std::string> unreadable;
    std::vector<std::string> duplicateNames;    // one name, two files

    bool provides(const std::string& name) const;
};

MaterialSet materialFiles(const std::filesystem::path& materialsDir);

// ── The textures a material needs, resolved for a registry-free runtime ─────
// A .material names its textures by SOURCE path ("assets/tex/brick.png"). A
// dist has no registry.db, so that path resolves to nothing there and every
// textured material binds its white fallback — the same failure meshes had
// before their sibling .ctex files shipped.
//
// So the packager resolves each source path through the registry it CAN see
// (the dev machine's), reports the cooked files to copy, and rewrites the
// material's `cooked` field to a cache-relative path the runtime can use with
// no registry at all.
//
// Done here rather than in the cooker because a cooker may run in a WORKER
// PROCESS that receives only source/output/uuid on argv — it has no registry to
// resolve against. See MaterialTexture::cooked.
struct MaterialTextureSet {
    // Cache-relative cooked textures to ship ("textures/<uuid>.cooked"), sorted
    // and de-duplicated: several materials commonly share one image.
    std::vector<std::string> cookedRel;
    // Source paths no registry record could resolve. These ship WITHOUT a
    // texture and are worth a loud warning — it is precisely the silent
    // untextured-dist failure this exists to prevent.
    std::vector<std::string> unresolved;
};

// Resolves every texture of every material and writes the material, with its
// `cooked` fields filled, into `outDir` — the package's material directory.
//
// It writes to the PACKAGE, never back into the cache: cooked outputs are
// materialized from the DDC as read-only hardlinks, so the file in .cache is
// the same inode as the content-addressed blob. Rewriting it in place fails,
// and would corrupt a store entry shared with other projects if it did not.
// Callers therefore do NOT copy the .cmat separately — this call places it.
MaterialTextureSet resolveMaterialTextures(
        const MaterialSet& materials,
        assetlib::AssetRegistry& registry,
        const std::filesystem::path& cacheRoot,
        const std::filesystem::path& outDir);

} // namespace pkg
