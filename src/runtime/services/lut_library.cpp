// ── LutLibrary — implementation ─────────────────────────────────────────────
#include "runtime/services/lut_library.h"

#include <fstream>
#include <system_error>

#include "components/colour_grading.h"
#include "core/cube_lut.h"
#include "core/logger.h"

std::shared_ptr<const CubeLut> LutLibrary::get(const std::filesystem::path& projectRoot,
                                               const std::string& relPath) {
    namespace fs = std::filesystem;
    if (relPath.empty()) return nullptr;

    // A different project invalidates every relative path in the cache.
    if (projectRoot != m_root) {
        m_cache.clear();
        m_root = projectRoot;
    }
    if (auto it = m_cache.find(relPath); it != m_cache.end()) return it->second;

    // From here on this path is resolved exactly once; every early return below
    // leaves a null entry, which is what makes the failure report-once.
    std::shared_ptr<const CubeLut>& slot = m_cache[relPath];

    const fs::path rel = fs::path(relPath);
    const fs::path norm = rel.lexically_normal();
    if (rel.is_absolute() || rel.has_root_name() || norm.empty() ||
        *norm.begin() == "..") {
        LOG_WARN("Colour", "grading LUT '%s' refused: it must be a path inside the "
                 "project, not absolute and not climbing out with '..'",
                 relPath.c_str());
        return nullptr;
    }
    if (projectRoot.empty()) {
        LOG_WARN("Colour", "grading LUT '%s' ignored: no project is open to "
                 "resolve it against", relPath.c_str());
        return nullptr;
    }

    const fs::path full = projectRoot / norm;
    std::error_code ec;
    const uintmax_t bytes = fs::file_size(full, ec);
    if (ec) {
        LOG_WARN("Colour", "grading LUT '%s' could not be read (%s) — the view "
                 "renders ungraded", relPath.c_str(), ec.message().c_str());
        return nullptr;
    }
    if (bytes > kMaxFileBytes) {
        LOG_WARN("Colour", "grading LUT '%s' is %llu bytes, over the %llu MB limit "
                 "— refused", relPath.c_str(), (unsigned long long)bytes,
                 (unsigned long long)(kMaxFileBytes / (1024 * 1024)));
        return nullptr;
    }

    std::string text((size_t)bytes, '\0');
    std::ifstream in(full, std::ios::binary);
    if (!in.read(text.data(), (std::streamsize)bytes)) {
        LOG_WARN("Colour", "grading LUT '%s' could not be read — the view renders "
                 "ungraded", relPath.c_str());
        return nullptr;
    }

    auto lut = std::make_shared<CubeLut>();
    std::string err;
    if (!parseCubeLut(text, *lut, &err)) {
        LOG_WARN("Colour", "grading LUT '%s' refused: %s — the view renders "
                 "ungraded", relPath.c_str(), err.c_str());
        return nullptr;
    }
    LOG_INFO("Colour", "grading LUT '%s': %u³", relPath.c_str(), lut->size);
    slot = std::move(lut);
    return slot;
}

DisplayTransform resolveDisplayTransform(const ColourGrading* grading,
                                         LutLibrary& luts,
                                         const std::filesystem::path& projectRoot) {
    DisplayTransform t;
    if (!grading) return t;
    t.exposure   = resolvedExposure(*grading);
    t.toneMapper = resolvedToneMapper(*grading);
    t.lut        = luts.get(projectRoot, grading->lutPath);
    return t;
}
