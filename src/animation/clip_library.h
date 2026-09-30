#pragma once
// ── ClipLibrary — standalone animation clips as assets ───────────────────────
// Loads an animation clip from its OWN file (a Mixamo clip FBX: skeleton +
// animation tracks, no mesh) and BINDS it to a target skeleton by bone name.
// After binding, sampling is pure index math, with no per-frame string lookups.
// Tracks naming no bone of the target are skipped with a warning (that is the
// name-binding contract; retargeting across different rigs is a future upgrade
// of exactly this step). A clip bound to two skeletons is two registry entries:
// the cache key is (source path | skeleton handle).
//
// WHERE A CLIP COMES FROM, in order (WO-016):
//   1. the COOKED clip: the cook pipeline cooks every animation-only source to
//      a skeleton-independent anim::CookedClip (animation/cooked_clip.h),
//      whether or not anyone ever played it. The host's locator finds it (the
//      editor asks the asset registry); with none, it is the packaged file
//      <cacheRoot>/assetlib::packagedClipFileName(<project-relative source path>), which
//      engine_build writes for every cooked clip.
//   2. the SOURCE READER the host may install (dev builds): the file read
//      through the import front ends, for a clip dropped into the project
//      before its cook has finished. WO-018 replaces this with a pending job.
//   3. neither: an error, "clip not cooked".
//
// Binding is the same code either way (anim::bindRawClip). Nothing here parses
// a source format or writes a cache: the cook-on-first-bind cache this used to
// keep (.ozzclip, per TARGET skeleton) made a shipped build play only the clips
// someone had happened to play in the editor.
#include "animation/animation_clip.h"
#include "animation/clip_registry.h"
#include "animation/cooked_clip.h"
#include "animation/ozz_bridge.h"
#include "animation/skeleton.h"
#include "core/handle.h"
#include "core/logger.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <unordered_map>

class ClipLibrary {
public:
    // Reads the clip in `sourcePath` and binds it, by bone name, to `target`
    // (which has its ozz data). An invalid AnimClip if it cannot, having logged
    // why. mappedTracks of totalTracks says how much of the file's clip bound.
    using SourceReader = std::function<AnimClip(const std::string& sourcePath, const Skeleton& target)>;
    // The cooked clip for `sourcePath`, or an empty path if the host knows of
    // none (then the packaged location is tried).
    using CookedLocator = std::function<std::filesystem::path(const std::string& sourcePath)>;

    // Where packaged clips live: <project>/.cache/anim, set at project open.
    void setCacheRoot(const std::filesystem::path& dir) { m_cacheRoot = dir; }
    void setCookedLocator(CookedLocator locator) { m_locator = std::move(locator); }
    void setSourceReader(SourceReader reader) { m_reader = std::move(reader); }

    // Load (or return cached) `sourcePath` bound to `skeleton`.
    AnimClipHandle load(const std::string& sourcePath,
                        SkeletonHandle skelHandle, const Skeleton& skeleton,
                        AnimClipRegistry& clips) {
        const std::string key = sourcePath + "|" + std::to_string(skelHandle.id);
        if (auto it = m_cache.find(key); it != m_cache.end()) return it->second;
        if (!skeleton.ozz) {
            LOG_ERROR("Anim", "target skeleton has no ozz runtime data");
            return {};
        }
        const auto t0 = std::chrono::steady_clock::now();

        AnimClip clip;
        const char* from = nullptr;
        const std::filesystem::path cooked = cookedPathFor(sourcePath);
        if (!cooked.empty()) {
            anim::CookedClip c;
            std::string why;
            if (anim::readCookedClipFile(cooked, c, why)) {
                clip = anim::bindRawClip(c.trackBones, c.keys, skeleton);
                from = "cooked";
            } else {
                LOG_ERROR("Anim", "cooked clip for %s refused: %s", sourcePath.c_str(), why.c_str());
            }
        }
        if (!from && m_reader) {
            clip = m_reader(sourcePath, skeleton);
            from = "source";
        }
        if (!from) {
            // A shipping runtime has no reader: landing here means the clip was
            // never cooked, or was not packaged. A build bug, not a runtime
            // fallback opportunity.
            LOG_ERROR("Anim", "clip not cooked: %s — cook the project (the editor "
                      "does on open) and re-run engine_build", sourcePath.c_str());
            return {};
        }
        if (clip.totalTracks > 0 && clip.mappedTracks == 0) {
            LOG_ERROR("Anim", "'%s': no tracks match the target skeleton "
                      "(%d tracks, %d bones) — wrong rig?",
                      clip.name.c_str(), clip.totalTracks, skeleton.boneCount());
            return {};
        }
        if (!clip.valid()) return {};   // decode, reader or builder failure (already logged)
        if (clip.mappedTracks < clip.totalTracks)
            LOG_WARN("Anim", "'%s': %d/%d tracks unmapped on this skeleton",
                     clip.name.c_str(), clip.totalTracks - clip.mappedTracks,
                     clip.totalTracks);

        AnimClipHandle h = clips.add(std::move(clip));
        m_cache[key] = h;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        LOG_SUCCESS("Anim", "bound clip '%s' (%.2fs, %d/%d tracks) from %s [%s, %.1f ms]",
                    clips.get(h)->name.c_str(), clips.get(h)->duration,
                    clips.get(h)->mappedTracks, clips.get(h)->totalTracks,
                    std::filesystem::path(sourcePath).filename().string().c_str(), from, ms);
        return h;
    }

    // Registries reset (project switch / registry clear) — drop stale handles.
    void clear() { m_cache.clear(); }

private:
    // The host's answer first; else the packaged file, named from the path
    // relative to the project (the cache root is <project>/.cache/anim).
    std::filesystem::path cookedPathFor(const std::string& sourcePath) const {
        std::error_code ec;
        if (m_locator)
            if (auto p = m_locator(sourcePath); !p.empty() && std::filesystem::exists(p, ec)) return p;
        if (m_cacheRoot.empty()) return {};
        std::filesystem::path rel(sourcePath);
        if (rel.is_absolute()) {
            const auto projectRoot = m_cacheRoot.parent_path().parent_path();
            auto r = std::filesystem::relative(rel, projectRoot, ec);
            if (ec || r.empty() || r.native()[0] == '.') return {};
            rel = r;
        }
        const auto p = m_cacheRoot / assetlib::packagedClipFileName(rel.generic_string());
        return std::filesystem::exists(p, ec) ? p : std::filesystem::path{};
    }

    SourceReader          m_reader;      // unset = cooked clips only
    CookedLocator         m_locator;     // unset = packaged clips only
    std::filesystem::path m_cacheRoot;   // <project>/.cache/anim
    std::unordered_map<std::string, AnimClipHandle> m_cache; // "path|skelId"
};
