#pragma once
// ── LutLibrary — grading LUTs by path, parsed once ──────────────────────────
//
// Colour pipeline stage B3. A camera's ColourGrading names a .cube by project-
// relative path; this turns the name into a parsed LUT and keeps it.
//
// ── WHY NOT A COOKED ASSET (YET) ────────────────────────────────────────────
// §4 B3 says the cooker converts .cube files. A 33³ LUT is ~36 000 short text
// lines and parses in milliseconds, and it is loaded once per path per process,
// so the cook buys nothing measurable today — while a new cooker, cooked format
// and registry entry would be most of the stage. The parser is fuzzed exactly as
// a cooked format's reader would be (fuzz_cube_lut), so moving the parse into a
// cooker later changes WHERE it runs, not what it accepts.
//
// ── WHAT A LUT HERE MUST BE BUILT FOR ───────────────────────────────────────
// DISPLAY-REFERRED, sRGB-ENCODED input in [0,1]: the output pass grades AFTER
// the tone map and the sRGB encode (core/output_transform.h is that order, and
// colour_test pins the shader to it). A .cube whose declared domain reaches
// outside [0,1] was built for scene-linear or log input and is refused here with
// that said, rather than applied to values it was never designed for.
//
// ── A PATH FROM A SCENE FILE IS UNTRUSTED ───────────────────────────────────
// It must stay inside the project: absolute paths and any path that climbs out
// with `..` are refused before the filesystem is touched. Files over 64 MB are
// refused before they are read. Symlinks inside the project that point out of
// it are NOT detected — the same limit the script loader has.
//
// ── COST PER FRAME: NONE ────────────────────────────────────────────────────
// get() runs every frame for every graded view. The cache is keyed by the
// relative path string the component already holds, so a hit is a hash lookup
// with no allocation; failures are cached as null and reported ONCE, so a broken
// grade costs one log line rather than one per frame. Editing a .cube does not
// reload it — restart the session. Main thread only.
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "render/display_settings.h"

struct CubeLut;
struct ColourGrading;

class LutLibrary {
public:
    std::shared_ptr<const CubeLut> get(const std::filesystem::path& projectRoot,
                                       const std::string& relPath);
    size_t cachedCount() const { return m_cache.size(); }

    static constexpr uintmax_t kMaxFileBytes = 64u * 1024 * 1024;

private:
    std::filesystem::path                                           m_root;
    std::unordered_map<std::string, std::shared_ptr<const CubeLut>> m_cache;
};

// A camera's grading (or none) -> what the output pass needs.
DisplayTransform resolveDisplayTransform(const ColourGrading* grading,
                                         LutLibrary& luts,
                                         const std::filesystem::path& projectRoot);
