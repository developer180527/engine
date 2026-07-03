#pragma once
// AudioPlugin — miniaudio-backed IAudioService.
//
// Mirrors JoltPlugin's shape: an IEnginePlugin that ALSO implements a
// script-facing service interface (IAudioService) and publishes itself into
// the game world via an AudioServiceRef singleton on simulation start. The
// LuaScriptPlugin picks that up in bindAudio() and routes Audio.play(...)
// calls here. Until then every call safely no-ops inside ScriptHost.
#include "runtime/plugin.h"
#include "core/logger.h"
#include "core/memory/mem.h"
#include "runtime/runtime_context.h"
#include "runtime/scripting/script_services.h"
#include "miniaudio.h"

#include <flecs.h>
#include <unordered_map>
#include <string>
#include <filesystem>
#include <cstdio>

class AudioPlugin final : public IEnginePlugin, public IAudioService {
public:
    const char* name()    const override { return "Audio (miniaudio)"; }
    const char* version() const override { return "0.1.0"; }

    // ── Editor lifecycle ────────────────────────────────────────────────
    void onAttach(RuntimeContext& ctx) override {
        m_assetsRoot = ctx.project.assetsRoot;
        ma_engine_config cfg = ma_engine_config_init();
        // miniaudio → Audio heap (decoders, mixing buffers, voice objects).
        cfg.allocationCallbacks.onMalloc =
            [](size_t sz, void*) { return mem::alloc(sz, 16, mem::Tag::Audio); };
        cfg.allocationCallbacks.onRealloc =
            [](void* p, size_t sz, void*) {
                if (!p) return mem::alloc(sz, 16, mem::Tag::Audio);
                return mem::realloc(p, sz);
            };
        cfg.allocationCallbacks.onFree = [](void* p, void*) { mem::free(p); };
        if (ma_engine_init(&cfg, &m_engine) != MA_SUCCESS) {
            LOG_ERROR("Audio", "ma_engine_init failed - audio disabled");
            m_ready = false;
            return;
        }
        m_ready = true;
        LOG_SUCCESS("Audio", "miniaudio engine started");
    }

    void onDetach() override {
        if (!m_ready) return;
        stopAll();
        ma_engine_uninit(&m_engine);
        m_ready = false;
    }

    // ── Simulation lifecycle ────────────────────────────────────────────
    void onSimulationStart(flecs::world& gw) override {
        gw.set<AudioServiceRef>({ static_cast<IAudioService*>(this) });
    }
    void onSimulationStop() override { stopAll(); }

    void onUpdate(flecs::world& /*gw*/, float /*dt*/) override {
        if (!m_ready) return;
        cullFinished();
        // Listener follows the primary camera — added in the next pass once 2D
        // playback is confirmed. 3D playAt currently uses the default listener
        // at the origin facing -Z.
    }


    // ── IAudioService (invoked from Lua via ScriptHost) ─────────────────
    uint32_t play(const char* path) override {
        return spawn(path, /*spatial*/false, 0.f, 0.f, 0.f);
    }
    uint32_t playAt(const char* path, float x, float y, float z) override {
        return spawn(path, /*spatial*/true, x, y, z);
    }
    void stop(uint32_t handle) override {
        auto it = m_sounds.find(handle);
        if (it == m_sounds.end()) return;
        ma_sound_uninit(it->second);
        delete it->second;
        m_sounds.erase(it);
    }

private:
    uint32_t spawn(const char* path, bool spatial, float x, float y, float z) {
        if (!m_ready || !path || !*path) return 0;
        const std::string full = resolve(path);

        ma_uint32 flags = 0;
        if (!spatial) flags |= MA_SOUND_FLAG_NO_SPATIALIZATION;
        // Decode small clips for low-latency SFX; stream large files (music)
        // so a 60 MB track does not hitch or balloon memory on play.
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(full, ec);
        flags |= (!ec && bytes < (1u << 20)) ? MA_SOUND_FLAG_DECODE
                                             : MA_SOUND_FLAG_STREAM;

        ma_sound* snd = new ma_sound();
        if (ma_sound_init_from_file(&m_engine, full.c_str(), flags,
                                    nullptr, nullptr, snd) != MA_SUCCESS) {
            LOG_ERROR("Audio", "failed to load '%s'", full.c_str());
            delete snd;
            return 0;
        }
        if (spatial) ma_sound_set_position(snd, x, y, z);
        ma_sound_start(snd);

        const uint32_t h = ++m_nextHandle;
        m_sounds.emplace(h, snd);
        return h;
    }

    void cullFinished() {
        for (auto it = m_sounds.begin(); it != m_sounds.end(); ) {
            if (ma_sound_at_end(it->second)) {
                ma_sound_uninit(it->second);
                delete it->second;
                it = m_sounds.erase(it);
            } else {
                ++it;
            }
        }
    }

    void stopAll() {
        for (auto& [h, snd] : m_sounds) {
            ma_sound_uninit(snd);
            delete snd;
        }
        m_sounds.clear();
    }

    std::string resolve(const char* path) const {
        namespace fs = std::filesystem;
        fs::path p(path);
        if (p.is_absolute()) return p.string();
        if (!m_assetsRoot.empty()) return (m_assetsRoot / p).string();
        return std::string(path);
    }

    ma_engine                                m_engine{};
    bool                                     m_ready = false;
    std::filesystem::path                    m_assetsRoot;
    std::unordered_map<uint32_t, ma_sound*>  m_sounds;
    uint32_t                                 m_nextHandle = 0;
};
