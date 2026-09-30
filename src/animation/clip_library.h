#pragma once
// ── ClipLibrary — standalone animation clips as assets ───────────────────────
// Loads an animation clip from its OWN source file (a Mixamo clip FBX:
// skeleton + animation tracks, no mesh) and BINDS it to a target skeleton:
// track bone-names resolve through Skeleton::findBone, baking boneIndex into
// each channel — after binding, sampling is pure index math, zero per-frame
// string lookups. Unmapped tracks are skipped with a warning (that's the
// name-based binding contract; retargeting across different rigs is a future
// upgrade of exactly this step).
//
// A clip bound to two different skeletons is two registry entries — the cache
// key is (source path | skeleton handle).
//
// WHERE A CLIP COMES FROM is not this module's business (WO-015). A cooked clip
// is read here; an uncooked one comes from the SOURCE READER the host installs
// (setSourceReader): in dev builds, the runtime installs one that imports the
// file through the import front ends and binds its first clip
// (assets/clip_source.h). Without a reader (shipping builds, bare tools) an
// uncooked clip is an error: "clip not cooked". This header used to parse FBX
// with Assimp itself, which tied animation to one parser.
//
// THE COOKER (cook-on-first-bind): the expensive part of load() is the source
// parse. After the first successful bind, the built ozz Animation is
// serialized to <cacheRoot>/<hash(source|skeletonSig)>.ozzclip (ozz archive +
// a small invalidation header). Every later bind — including every later RUN —
// deserializes the archive instead (sub-millisecond, no parse). Invalidation:
// source file size+mtime, skeleton joint-name signature, cache version. Hosts
// call setCacheRoot at project open; without it (bare tools) cooking is off
// and the source reader stands alone.
#include "animation/animation_clip.h"
#include "animation/clip_registry.h"
#include "animation/skeleton.h"
#include "core/handle.h"
#include "core/logger.h"

#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/memory/unique_ptr.h>

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

    // Installed by the host where source files may be read (dev builds).
    // Unset, a clip that is not cooked cannot load.
    void setSourceReader(SourceReader reader) { m_reader = std::move(reader); }

    // Where cooked clips live (host sets <project>/.cache/anim at project
    // open). Empty = cooking disabled.
    void setCacheRoot(const std::filesystem::path& dir) {
        m_cacheRoot = dir;
        if (!m_cacheRoot.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(m_cacheRoot, ec);
        }
    }

    // Load (or return cached) `sourcePath` bound to `skeleton`: the cooked clip
    // if there is one, else the source reader's.
    AnimClipHandle load(const std::string& sourcePath,
                        SkeletonHandle skelHandle, const Skeleton& skeleton,
                        AnimClipRegistry& clips) {
        const std::string key = sourcePath + "|" + std::to_string(skelHandle.id);
        if (auto it = m_cache.find(key); it != m_cache.end()) return it->second;

        const auto t0 = std::chrono::steady_clock::now();

        // ── Cooked fast path ────────────────────────────────────────────────
        if (AnimClipHandle h = loadCooked(sourcePath, skeleton, clips); h.valid()) {
            m_cache[key] = h;
            return h;
        }

        // ── Not cooked: the host's source reader, if it installed one ────────
        if (!m_reader) {
            // A shipping runtime has none: landing here means the project was
            // packaged without cooking this clip. A packaging bug, not a
            // runtime fallback opportunity.
            LOG_ERROR("Anim", "clip not cooked: %s — this build reads no source "
                      "files; re-run engine_build / cook in the editor", sourcePath.c_str());
            return {};
        }
        if (!skeleton.ozz) {
            LOG_ERROR("Anim", "target skeleton has no ozz runtime data");
            return {};
        }
        AnimClip clip = m_reader(sourcePath, skeleton);
        if (clip.totalTracks > 0 && clip.mappedTracks == 0) {
            LOG_ERROR("Anim", "'%s': no tracks match the target skeleton "
                      "(%d tracks, %d bones) — wrong rig?",
                      clip.name.c_str(), clip.totalTracks, skeleton.boneCount());
            return {};
        }
        if (!clip.valid()) return {};   // reader or builder failure (already logged)
        if (clip.mappedTracks < clip.totalTracks)
            LOG_WARN("Anim", "'%s': %d/%d tracks unmapped on this skeleton",
                     clip.name.c_str(), clip.totalTracks - clip.mappedTracks,
                     clip.totalTracks);

        AnimClipHandle h = clips.add(std::move(clip));
        m_cache[key] = h;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        LOG_SUCCESS("Anim", "bound clip '%s' (%.2fs, %d/%d tracks) from %s "
                    "[source, %.1f ms]",
                    clips.get(h)->name.c_str(), clips.get(h)->duration,
                    clips.get(h)->mappedTracks, clips.get(h)->totalTracks,
                    std::filesystem::path(sourcePath).filename().string().c_str(),
                    ms);
        saveCooked(sourcePath, skeleton, *clips.get(h));
        return h;
    }

    // Registries reset (project switch / registry clear) — drop stale handles.
    void clear() { m_cache.clear(); }

private:
    // ── Cooked-clip format ──────────────────────────────────────────────────
    // [CookedHeader][ozz OArchive(Animation)] in one file. The header carries
    // everything invalidation needs; name/duration live inside the archive.
    static constexpr uint32_t kCookMagic   = 0x4C435A4F;   // 'OZCL'
    static constexpr uint32_t kCookVersion = 2;   // v2: names inside archives
    struct CookedHeader {
        uint32_t magic, version;
        uint64_t srcSize;
        int64_t  srcMtime;
        uint64_t skelSig;
        int32_t  mappedTracks, totalTracks;
    };

    // Joint-name signature: same rig => same cooked clip, changed rig =>
    // stale. Order-sensitive FNV over the ozz skeleton's joint names.
    static uint64_t skeletonSig(const Skeleton& skeleton) {
        uint64_t h = 1469598103934665603ull;
        for (const char* n : skeleton.ozz->joint_names())
            for (const char* c = n; *c; ++c)
                h = (h ^ (uint64_t)(uint8_t)*c) * 1099511628211ull;
        return h;
    }

    // Cache key path: PROJECT-RELATIVE and separator-normalized. Hashing the
    // raw string made cache filenames machine-specific — a clip cooked on a
    // dev machine (absolute /Users/.../assets/x.fbx) could never be found by
    // a shipped build running from dist/ (different root, or the relative
    // "assets/x.fbx" the scene stores). Relative keys travel with the
    // project. projectRoot derives from the cache dir (<root>/.cache/anim).
    std::string cacheKeyPath(const std::string& sourcePath) const {
        std::filesystem::path p(sourcePath);
        if (p.is_absolute() && !m_cacheRoot.empty()) {
            const auto projectRoot =
                m_cacheRoot.parent_path().parent_path();
            std::error_code ec;
            auto rel = std::filesystem::relative(p, projectRoot, ec);
            if (!ec && !rel.empty() && rel.native()[0] != '.')
                p = rel;
        }
        return p.generic_string();
    }

    std::filesystem::path cookedPath(const std::string& sourcePath,
                                     uint64_t skelSig) const {
        const std::string key = cacheKeyPath(sourcePath);
        uint64_t h = 1469598103934665603ull;
        for (char c : key) h = (h ^ (uint64_t)(uint8_t)c) * 1099511628211ull;
        h ^= skelSig;
        char name[32];
        std::snprintf(name, sizeof(name), "%016llx.ozzclip",
                      (unsigned long long)h);
        return m_cacheRoot / name;
    }

    static bool srcStat(const std::string& p, uint64_t* size, int64_t* mtime) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(p, ec);
        if (ec) return false;
        const auto mt = std::filesystem::last_write_time(p, ec);
        if (ec) return false;
        *size  = (uint64_t)sz;
        *mtime = (int64_t)mt.time_since_epoch().count();
        return true;
    }

    AnimClipHandle loadCooked(const std::string& sourcePath,
                              const Skeleton& skeleton,
                              AnimClipRegistry& clips) {
        if (m_cacheRoot.empty() || !skeleton.ozz) return {};
        const auto t0 = std::chrono::steady_clock::now();
        // Source stat is for INVALIDATION (dev iteration). A shipped build
        // deliberately doesn't carry source FBX — an absent source means
        // the cooked clip IS the asset: trust it. (Stat-fail used to reject
        // outright, which broke every shipped clip.)
        uint64_t srcSize = 0; int64_t srcMtime = 0;
        const bool haveSrc = srcStat(sourcePath, &srcSize, &srcMtime);
        const uint64_t sig = skeletonSig(skeleton);
        const auto path = cookedPath(sourcePath, sig);

        ozz::io::File file(path.string().c_str(), "rb");
        if (!file.opened()) return {};
        CookedHeader hdr{};
        if (file.Read(&hdr, sizeof(hdr)) != sizeof(hdr)) return {};
        if (hdr.magic != kCookMagic || hdr.version != kCookVersion ||
            hdr.skelSig != sig)
            return {};   // wrong format/rig — dev re-cooks
        if (haveSrc && (hdr.srcSize != srcSize || hdr.srcMtime != srcMtime))
            return {};   // source changed — dev re-cooks

        ozz::io::IArchive archive(&file);
        if (!archive.TestTag<ozz::animation::Animation>()) return {};
        auto animation = ozz::make_unique<ozz::animation::Animation>();
        archive >> *animation;

        AnimClip clip;
        clip.name         = animation->name();
        clip.duration     = animation->duration();
        clip.mappedTracks = hdr.mappedTracks;
        clip.totalTracks  = hdr.totalTracks;
        clip.ozz = std::shared_ptr<const ozz::animation::Animation>(
            animation.release(), ozz::Deleter<ozz::animation::Animation>());

        AnimClipHandle h = clips.add(std::move(clip));
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        LOG_SUCCESS("Anim", "bound clip '%s' (%.2fs, %d/%d tracks) from %s "
                    "[COOKED, %.2f ms]",
                    clips.get(h)->name.c_str(), clips.get(h)->duration,
                    hdr.mappedTracks, hdr.totalTracks,
                    std::filesystem::path(sourcePath).filename().string().c_str(),
                    ms);
        return h;
    }

    void saveCooked(const std::string& sourcePath, const Skeleton& skeleton,
                    const AnimClip& clip) {
        if (m_cacheRoot.empty() || !skeleton.ozz || !clip.ozz) return;
        uint64_t srcSize; int64_t srcMtime;
        if (!srcStat(sourcePath, &srcSize, &srcMtime)) return;
        const uint64_t sig  = skeletonSig(skeleton);
        const auto     path = cookedPath(sourcePath, sig);

        ozz::io::File file(path.string().c_str(), "wb");
        if (!file.opened()) {
            LOG_WARN("Anim", "cook write failed: %s", path.string().c_str());
            return;
        }
        CookedHeader hdr{kCookMagic, kCookVersion, srcSize, srcMtime, sig,
                         clip.mappedTracks, clip.totalTracks};
        file.Write(&hdr, sizeof(hdr));
        ozz::io::OArchive archive(&file);
        archive << *clip.ozz;
        LOG_INFO("Anim", "cooked '%s' -> %s", clip.name.c_str(),
                 path.filename().string().c_str());
    }

    SourceReader          m_reader;      // unset = cooked clips only
    std::filesystem::path m_cacheRoot;   // empty = cooking off
    std::unordered_map<std::string, AnimClipHandle> m_cache; // "path|skelId"
};
