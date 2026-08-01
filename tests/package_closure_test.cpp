// ── package_closure_test — what a dist must contain ────────────────────────
//
// The packaging path had no test, and it shipped a completely broken game:
// every mesh present, the game running, EVERY SURFACE UNTEXTURED. It came from
// an ordinary flow — a clean rebuild against a warm shared DDC — which is
// exactly what a CI runner or a second dev machine does.
//
// The cause was a filename heuristic. engine_build copied a mesh's sibling
// .ctex by guessing "<meshUuid>_t*.ctex". That holds only while a mesh and its
// textures are cooked together in one pass; the DDC restores siblings under the
// names they were FIRST written with, while the mesh output takes the CURRENT
// uuid, so the prefixes stop matching.
//
// So the central case here is deliberately the one the old code got wrong: a
// cooked mesh whose texture references DO NOT share its filename prefix. If
// meshClosure ever goes back to guessing, this fails.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

#include <assetlib/mesh_asset.h>
#include <assetlib/scene_asset.h>
#include <assetlib/shader_asset.h>

#include "tools/packaging/package_closure.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

namespace fs = std::filesystem;

static void setPath(char (&dst)[512], const char* s) {
    std::snprintf(dst, sizeof(dst), "%s", s);
}

// A minimal but REAL cooked mesh — written through assetlib::saveMesh, so this
// exercises the same reader the packager uses rather than a mock of it.
static bool writeMesh(const fs::path& p, const char* baseColor,
                      const char* normalMap) {
    // No geometry: the reader derives section offsets from the header's counts,
    // so writing bytes without setting them would put the material section at
    // the wrong offset. The closure cares only about material references.
    assetlib::MeshAsset ma;
    assetlib::CookedMaterial mat;
    if (baseColor) {
        setPath(mat.baseColorPath, baseColor);
        mat.flags |= assetlib::kMatFlag_HasBaseColor;
    }
    if (normalMap) {
        setPath(mat.normalMapPath, normalMap);
        mat.flags |= assetlib::kMatFlag_HasNormalMap;
    }
    ma.materials.push_back(mat);
    // The header's counts are what the reader trusts; saveMesh does not derive
    // them from the vectors.
    ma.header.materialCount = (uint32_t)ma.materials.size();
    return assetlib::saveMesh(ma, p);
}

static void touch(const fs::path& p) {
    std::ofstream f(p, std::ios::binary);
    f << "ctex";
}

// A real cooked scene whose entities reference the given mesh paths. Written
// through assetlib::saveScene so the same reader the packager uses is exercised.
static bool writeScene(const fs::path& p,
                       const std::vector<std::string>& meshRefs) {
    assetlib::SceneAsset sc;
    for (const auto& ref : meshRefs) {
        assetlib::SceneEntity e;
        const auto [off, len] = assetlib::stringTableAppend(sc.stringTable, ref);
        e.meshCookedOffset = off;
        e.meshCookedLength = len;
        sc.entities.push_back(e);
    }
    sc.header.entityCount     = (uint32_t)sc.entities.size();
    sc.header.stringTableSize = (uint32_t)sc.stringTable.size();
    return assetlib::saveScene(sc, p);
}

// A real cooked shader carrying `name`, written through assetlib::saveShader.
static bool writeShader(const fs::path& p, const std::string& name) {
    assetlib::ShaderAsset sh;
    sh.name = name;
    sh.blob.assign(16, 0);
    sh.variants.push_back({ 0, assetlib::kProfileMetal, 0, 8, 8, 8 });
    return assetlib::saveShader(sh, p);
}

static bool listed(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

static bool has(const pkg::MeshClosure& c, const std::string& filename) {
    for (const auto& f : c.files) if (f.filename() == filename) return true;
    return false;
}

int main() {
    std::printf("package_closure_test\n");

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "engine_pkg_test";
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // ── THE REGRESSION ──────────────────────────────────────────────────────
    // Mesh uuid and texture uuids deliberately differ, exactly as they do after
    // the DDC materializes a cache into a re-scanned project.
    {
        const fs::path mesh = dir / "3daba53b-mesh.cooked";
        CHECK(writeMesh(mesh, "a6e9e252-tex_t0.ctex", "a6e9e252-tex_t1.ctex"),
              "wrote a cooked mesh whose textures do NOT share its prefix");
        touch(dir / "a6e9e252-tex_t0.ctex");
        touch(dir / "a6e9e252-tex_t1.ctex");

        const auto c = pkg::meshClosure(mesh);
        CHECK(!c.unreadable, "a real cooked mesh reads back");
        CHECK(c.files.size() == 3, "mesh + 2 textures ship (%zu)", c.files.size());
        CHECK(c.files[0] == mesh, "the mesh itself is first");
        // This is the assertion the old prefix heuristic failed. It shipped the
        // mesh and neither texture, and nothing anywhere reported a problem.
        CHECK(has(c, "a6e9e252-tex_t0.ctex") && has(c, "a6e9e252-tex_t1.ctex"),
              "textures ship even though their names share NO prefix with the "
              "mesh — the DDC-materialized case");
        CHECK(c.missing.empty(), "nothing reported missing");
    }

    // ── a missing texture is reported, never silently dropped ───────────────
    {
        const fs::path mesh = dir / "missing-tex.cooked";
        writeMesh(mesh, "not-on-disk_t0.ctex", nullptr);
        const auto c = pkg::meshClosure(mesh);
        CHECK(c.files.size() == 1, "only the mesh ships (%zu)", c.files.size());
        CHECK(c.missing.size() == 1 && c.missing[0] == "not-on-disk_t0.ctex",
              "the absent texture is NAMED, so the build is visibly suspect");
        CHECK(!c.unreadable, "a mesh with a dangling reference is still readable");
    }

    // ── a texture shared by two meshes is not duplicated ────────────────────
    // The cooker content-dedups siblings, so one .ctex referenced by several
    // meshes is the normal case, not an edge case.
    {
        touch(dir / "shared_t0.ctex");
        const fs::path a = dir / "mesh-a.cooked";
        const fs::path b = dir / "mesh-b.cooked";
        writeMesh(a, "shared_t0.ctex", nullptr);
        writeMesh(b, "shared_t0.ctex", nullptr);
        const auto ca = pkg::meshClosure(a);
        const auto cb = pkg::meshClosure(b);
        CHECK(ca.files.size() == 2 && cb.files.size() == 2,
              "each mesh claims the shared texture");
        CHECK(ca.files[1].filename() == cb.files[1].filename(),
              "...and it is the same file, so the copy dedups by name");
    }

    // One mesh naming the SAME texture for both slots must not list it twice —
    // a duplicate copy is harmless but a duplicate count makes the build log
    // lie about what shipped.
    {
        const fs::path mesh = dir / "same-both-slots.cooked";
        writeMesh(mesh, "shared_t0.ctex", "shared_t0.ctex");
        const auto c = pkg::meshClosure(mesh);
        CHECK(c.files.size() == 2,
              "one texture in two slots is listed once (%zu)", c.files.size());
    }

    // ── a mesh with no textures at all ──────────────────────────────────────
    {
        const fs::path mesh = dir / "untextured.cooked";
        writeMesh(mesh, nullptr, nullptr);
        const auto c = pkg::meshClosure(mesh);
        CHECK(c.files.size() == 1 && c.missing.empty(),
              "an untextured mesh ships alone, with no warnings");
    }

    // ── unreadable inputs ───────────────────────────────────────────────────
    {
        const auto absent = pkg::meshClosure(dir / "does-not-exist.cooked");
        CHECK(absent.unreadable && absent.files.empty(),
              "a missing mesh is unreadable, not an empty success");

        const fs::path garbage = dir / "garbage.cooked";
        { std::ofstream f(garbage, std::ios::binary); f << "not a mesh at all"; }
        const auto bad = pkg::meshClosure(garbage);
        CHECK(bad.unreadable && bad.files.empty(),
              "a corrupt mesh is unreadable — it must NOT ship as if it were "
              "fine, or the game loads nothing at that entity");
    }

    // ── a reference must not escape the cooked directory ────────────────────
    // References are basenames by contract, but a cooked blob can arrive from a
    // shared DDC — i.e. from another machine — so a path-shaped one is treated
    // as untrusted and reduced to its filename.
    {
        touch(dir / "escape_t0.ctex");
        const fs::path mesh = dir / "traversal.cooked";
        writeMesh(mesh, "../../../etc/escape_t0.ctex", nullptr);
        const auto c = pkg::meshClosure(mesh);
        CHECK(c.files.size() == 2
              && c.files[1] == dir / "escape_t0.ctex",
              "a ../ reference resolves inside the cooked directory, not above it");
    }

    // ═══ which meshes ship at all ═══════════════════════════════════════════
    // A bug here drops whole OBJECTS rather than just their textures — strictly
    // worse than the untextured failure, and the old inline version was just as
    // silent about it.
    const fs::path scenes = dir / "scenes";
    fs::create_directories(scenes, ec);
    {
        CHECK(writeScene(scenes / "main.cooked",
                         { "meshs/aaa.cooked", "meshs/bbb.cooked" }),
              "wrote a cooked scene referencing 2 meshes");
        CHECK(writeScene(scenes / "level2.cooked",
                         { "meshs/bbb.cooked", "meshs/ccc.cooked" }),
              "wrote a second scene sharing one of them");

        const auto r = pkg::sceneMeshRefs(scenes);
        CHECK(r.scenesRead == 2, "both scenes read (%zu)", r.scenesRead);
        CHECK(r.unreadableScenes.empty(), "neither is reported unreadable");
        // bbb appears in both scenes; shipping it twice would be a wasted copy
        // and a build log that lies about the package contents.
        CHECK(r.meshes.size() == 3, "3 distinct meshes (%zu)", r.meshes.size());
        CHECK(listed(r.meshes, "meshs/aaa.cooked")
              && listed(r.meshes, "meshs/bbb.cooked")
              && listed(r.meshes, "meshs/ccc.cooked"),
              "every referenced mesh is collected, across scenes");

        // Deterministic output: directory_iterator order is filesystem-defined,
        // and a package that differs run to run makes "did this build change?"
        // unanswerable.
        //
        // The scenes are written in REVERSE alphabetical order on purpose. On a
        // filesystem that returns creation order, unsorted iteration yields
        // zzz's mesh first and this catches it. On one that returns sorted
        // order it passes either way — so this is real coverage on some hosts
        // and vacuous on others, never a false failure. Stated because a test
        // that cannot fail is worse than no test if you believe it can.
        const fs::path ordered = dir / "scenes_ordered";
        fs::create_directories(ordered, ec);
        writeScene(ordered / "zzz.cooked", { "meshs/from-zzz.cooked" });
        writeScene(ordered / "aaa.cooked", { "meshs/from-aaa.cooked" });
        const auto o = pkg::sceneMeshRefs(ordered);
        CHECK(o.meshes.size() == 2 && o.meshes[0] == "meshs/from-aaa.cooked",
              "scenes are walked in sorted order, not filesystem order");

        const auto again = pkg::sceneMeshRefs(scenes);
        CHECK(again.meshes == r.meshes, "repeated calls agree");
    }

    // THE SILENT ONE: a scene that will not load must be REPORTED. The old
    // version `continue`d past it, so one corrupt or version-bumped scene
    // removed every object it contained from the package with no output.
    {
        { std::ofstream f(scenes / "corrupt.cooked", std::ios::binary);
          f << "this is not a scene"; }
        const auto r = pkg::sceneMeshRefs(scenes);
        CHECK(r.unreadableScenes.size() == 1
              && r.unreadableScenes[0] == "corrupt.cooked",
              "an unreadable scene is NAMED, not skipped in silence");
        CHECK(r.scenesRead == 2 && r.meshes.size() == 3,
              "...and the readable scenes still contribute their meshes");
        fs::remove(scenes / "corrupt.cooked", ec);
    }

    // Entities without a mesh (lights, cameras, spawn points) are normal and
    // must not produce empty reference strings.
    {
        const fs::path only = dir / "scenes_meshless";
        fs::create_directories(only, ec);
        writeScene(only / "lights.cooked", { "", "meshs/zzz.cooked", "" });
        const auto r = pkg::sceneMeshRefs(only);
        CHECK(r.meshes.size() == 1 && r.meshes[0] == "meshs/zzz.cooked",
              "meshless entities are skipped, not collected as empty strings");
    }

    // Non-.cooked files in the scenes directory are ignored, and a missing
    // directory is empty rather than a crash.
    {
        { std::ofstream f(scenes / "notes.txt"); f << "hello"; }
        const auto r = pkg::sceneMeshRefs(scenes);
        CHECK(r.scenesRead == 2, "non-.cooked files are ignored (%zu read)",
              r.scenesRead);

        const auto none = pkg::sceneMeshRefs(dir / "no_such_dir");
        CHECK(none.meshes.empty() && none.scenesRead == 0
              && none.unreadableScenes.empty(),
              "an absent scenes directory yields nothing, and does not throw");
    }

    // ═══ cooked shaders ═════════════════════════════════════════════════════
    // The runtime resolves these by the NAME inside each file, so a package can
    // hold shader files and still leave the game on compiled-in fallbacks —
    // which renders correctly, so nothing looks wrong.
    const fs::path shaderDir = dir / "shaders";
    fs::create_directories(shaderDir, ec);
    {
        CHECK(writeShader(shaderDir / "b-uuid.cooked", "standard"),
              "wrote a cooked shader declaring \"standard\"");
        CHECK(writeShader(shaderDir / "a-uuid.cooked", "unlit"),
              "wrote a second declaring \"unlit\"");

        const auto s = pkg::shaderFiles(shaderDir);
        CHECK(s.files.size() == 2, "both files ship (%zu)", s.files.size());
        CHECK(s.unreadable.empty() && s.duplicateNames.empty(),
              "nothing reported unreadable or duplicated");
        // THE assertion that matters: the name the pipeline asks for is present.
        CHECK(s.provides("standard"),
              "the package provides \"standard\" — without it the player falls "
              "back to compiled-in shaders and custom shading never runs");
        CHECK(s.provides("unlit") && !s.provides("nope"),
              "provides() answers by declared name, not filename");
        // Sorted by FILENAME, so the package is byte-identical run to run.
        // Asserted as is_sorted rather than by naming an element: that is the
        // exact contract a caller depends on, and it holds whatever order the
        // filesystem hands back — unlike an element check, which passes by luck
        // on a filesystem that already returns sorted entries.
        CHECK(std::is_sorted(s.files.begin(), s.files.end()),
              "files come out sorted for a deterministic package");
        // Same trick as the scene ordering check: a fresh directory written in
        // reverse alphabetical order, so a filesystem that returns creation
        // order exposes a missing sort. Vacuous on one that returns sorted
        // entries — real coverage on some hosts, never a false failure.
        const fs::path ord = dir / "shaders_ordered";
        fs::create_directories(ord, ec);
        writeShader(ord / "zzz.cooked", "z");
        writeShader(ord / "aaa.cooked", "a");
        const auto os = pkg::shaderFiles(ord);
        CHECK(os.files.size() == 2
              && std::is_sorted(os.files.begin(), os.files.end()),
              "reverse-written files still come out sorted");
        CHECK(s.names.size() == 2 && s.names[0] == "standard",
              "names are sorted independently (%zu)", s.names.size());
    }

    // A corrupt .cshader is NAMED, not shipped as dead weight that provides
    // nothing.
    {
        { std::ofstream f(shaderDir / "corrupt.cooked", std::ios::binary);
          f << "not a shader"; }
        const auto s = pkg::shaderFiles(shaderDir);
        CHECK(s.unreadable.size() == 1 && s.unreadable[0] == "corrupt.cooked",
              "an unreadable cooked shader is named");
        CHECK(s.files.size() == 2 && s.provides("standard"),
              "...and the good ones still ship");
        fs::remove(shaderDir / "corrupt.cooked", ec);
    }

    // Two files declaring one name: normally a stale cook left behind by an
    // older cooker version. One silently shadows the other at runtime.
    {
        writeShader(shaderDir / "z-stale.cooked", "standard");
        const auto s = pkg::shaderFiles(shaderDir);
        CHECK(s.duplicateNames.size() == 1 && s.duplicateNames[0] == "standard",
              "a duplicated shader name is reported");
        CHECK(s.files.size() == 3,
              "both copies still ship — dropping one would make the package "
              "depend on directory order (%zu)", s.files.size());
        fs::remove(shaderDir / "z-stale.cooked", ec);
    }

    // The silent case this whole struct exists for: shader files present, but
    // not the one the pipeline asks for.
    {
        const fs::path only = dir / "shaders_no_standard";
        fs::create_directories(only, ec);
        writeShader(only / "custom.cooked", "toon");
        const auto s = pkg::shaderFiles(only);
        CHECK(s.files.size() == 1 && !s.provides("standard"),
              "a package with shaders but no \"standard\" is detectable");
    }

    // Non-.cooked files ignored; an absent directory is empty, not a throw.
    {
        { std::ofstream f(shaderDir / "readme.txt"); f << "hi"; }
        CHECK(pkg::shaderFiles(shaderDir).files.size() == 2,
              "non-.cooked files are ignored");
        const auto none = pkg::shaderFiles(dir / "no_shaders_here");
        CHECK(none.files.empty() && none.names.empty()
              && !none.provides("standard"),
              "an absent shader directory provides nothing, and does not throw");
    }

    fs::remove_all(dir, ec);

    if (g_failures) {
        std::printf("package_closure_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("package_closure_test: PASS\n");
    return 0;
}
