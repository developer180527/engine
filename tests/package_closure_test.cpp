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
#include <string>

#include <assetlib/mesh_asset.h>

#include "tools/build/package_closure.h"

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

    fs::remove_all(dir, ec);

    if (g_failures) {
        std::printf("package_closure_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("package_closure_test: PASS\n");
    return 0;
}
