#pragma once
// AudioPlugin — IAudioService on top of EngineAudioProviderV1.
//
// This used to BE the audio engine: it called ma_engine_init, held ma_sound*
// objects and knew what a decoder was. It now knows none of that. Every audio
// operation goes through the provider table, and the only miniaudio reference
// left in the engine is the one line in audio_host_services.h that names the
// entry point.
//
// That is the swap seam made real. Replacing miniaudio with an FMOD or Wwise
// adapter, or a Rust spatial engine, is a change to which table this file asks
// for — not a change to this file.
//
// ── What was preserved from the miniaudio version, deliberately ─────────────
//
//   * mem::Tag::Audio. The old code pointed miniaudio's allocation callbacks at
//     the audio heap with hardcoded lambdas. The provider now receives the same
//     heap through host services, so the tagging survives the swap instead of
//     being a property of one backend.
//
//   * The device does not start on the main thread. Sampling engine_host
//     startup put 585 of 1722 main-thread samples — 34%, ~536 ms — inside
//     ma_engine_init, all of it BLOCKED in CoreAudio's
//     HALB_IOThread::StartAndWaitForState. Nothing on screen depends on an
//     audio device, so create() runs on a job and m_ready gates everything
//     until it lands. Silent audio for the first fraction of a second beats a
//     stalled boot, and this is where a game shows a logo anyway.
//
// ── What got BETTER as a side effect ────────────────────────────────────────
// The old path was ma_sound_init_from_file per play: every shot re-opened and
// re-decoded the same file. Sounds are now created once and cached by path, so
// repeat plays cost a voice rather than a decode. Large files use the pull
// streaming path, so a 60 MB music track is never resident at all — the old
// code streamed it too, but only because miniaudio was reading the file itself.
#include "runtime/plugin.h"
#include "core/logger.h"
#include "runtime/runtime_context.h"
#include "runtime/scripting/script_services.h"
#include "audio/audio_host_services.h"
#include "runtime/jobs/jobs.h"

#include <flecs.h>
#include <atomic>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class AudioPlugin final : public IEnginePlugin, public IAudioService {
public:
    const char* name()    const override { return "Audio (provider)"; }
    const char* version() const override { return "0.2.0"; }

    // ── Editor lifecycle ────────────────────────────────────────────────
    void onAttach(RuntimeContext& ctx) override {
        m_assetsRoot = ctx.project.assetsRoot;
        m_provider = engineAudioProviderV1();
        if (!m_provider) { LOG_ERROR("Audio", "no audio provider available"); return; }
        if (m_provider->version != ENGINE_AUDIO_PROVIDER_V) {
            LOG_ERROR("Audio", "provider reports v%u, engine expects v%u — disabled",
                      m_provider->version, (unsigned)ENGINE_AUDIO_PROVIDER_V);
            m_provider = nullptr;
            return;
        }
        m_host = audio::hostServices();

        EngineAudioDeviceDesc desc{};
        desc.structSize = (uint32_t)sizeof(desc);
        // Zeros mean "provider chooses", which is right: the device's own
        // preferred rate avoids a resample of everything we ever play.

        auto open = [this, desc]() {
            void* inst = nullptr;
            const EngineAudioResult r = m_provider->create(&desc, &m_host, &inst);
            if (r == ENGINE_AUDIO_E_NO_DEVICE) {
                // Expected on CI and dedicated servers. Not a failure.
                LOG_INFO("Audio", "no output device — running silent");
                return;
            }
            if (r != ENGINE_AUDIO_OK || !inst) {
                LOG_ERROR("Audio", "provider create failed (%d) — audio disabled", r);
                return;
            }
            m_self = inst;
            m_ready.store(true, std::memory_order_release);
            EngineAudioStats st{}; st.structSize = (uint32_t)sizeof(st);
            m_provider->getStats(inst, &st);
            LOG_SUCCESS("Audio", "provider started (%u Hz, %u-frame buffer)",
                        st.sampleRate, st.bufferFrames);
        };

        if (jobs::initialized()) m_openJob = jobs::run("audio.deviceStart", open);
        else                     open();   // tools and tests: same behaviour
    }

    void onDetach() override {
        // The open job writes m_self, so it MUST finish before teardown —
        // otherwise shutdown races a device coming up and tears down under it.
        if (m_openJob.valid()) { jobs::wait(m_openJob); m_openJob = {}; }
        if (!m_self) { reset(); return; }
        m_ready.store(false, std::memory_order_release);
        for (auto& [path, s] : m_sounds) m_provider->destroySound(m_self, s);
        m_provider->destroy(m_self);
        reset();
    }

    // ── Simulation lifecycle ────────────────────────────────────────────
    void onSimulationStart(flecs::world& gw) override {
        gw.set<AudioServiceRef>({ static_cast<IAudioService*>(this) });
    }
    void onSimulationStop() override { stopAll(); }

    void onUpdate(flecs::world& /*gw*/, float /*dt*/) override {
        // Voices are reaped by the provider — the engine finds its id stale next
        // time it uses one, which the ABI says is normal. Nothing to cull here.
        //
        // The listener still sits at the origin facing -Z; wiring it to the
        // primary camera is a scene concern and lands with the emitter batch.
    }

    // ── IAudioService (invoked from Lua via ScriptHost) ─────────────────
    uint32_t play(const char* path) override {
        return spawn(path, /*spatial*/false, 0.f, 0.f, 0.f);
    }
    uint32_t playAt(const char* path, float x, float y, float z) override {
        return spawn(path, /*spatial*/true, x, y, z);
    }
    void stop(uint32_t handle) override {
        if (!live()) return;
        auto it = m_voices.find(handle);
        if (it == m_voices.end()) return;
        m_provider->stop(m_self, it->second, 0);
        m_voices.erase(it);
    }

private:
    bool live() const { return m_self && m_ready.load(std::memory_order_acquire); }

    // Files at or above this are streamed rather than decoded. Below it, a
    // decode costs a little memory once and every later play is free; above it,
    // a decode would cost far more resident memory than the file itself.
    static constexpr uint64_t kStreamThreshold = 1u << 20;   // 1 MiB

    uint32_t spawn(const char* path, bool spatial, float x, float y, float z) {
        if (!live() || !path || !*path) return 0;
        const EngineSoundId sound = acquire(path);
        if (sound == ENGINE_AUDIO_NO_SOUND) return 0;

        EngineAudioPlayDesc d{};
        d.structSize = (uint32_t)sizeof(d);
        d.sound      = sound;
        d.volume     = 1.0f;
        d.pitch      = 1.0f;
        d.flags      = spatial ? ENGINE_AUDIO_F_SPATIAL : 0u;
        if (spatial) { d.position[0] = x; d.position[1] = y; d.position[2] = z; }

        const EngineVoiceId v = m_provider->play(m_self, &d);
        if (v == ENGINE_AUDIO_NO_VOICE) return 0;

        const uint32_t h = ++m_nextHandle;
        m_voices.emplace(h, v);
        // Bounded, so a long session cannot accumulate dead handles: the
        // provider runs far fewer voices than this, so anything this old has
        // certainly finished.
        m_handleOrder.push_back(h);
        while (m_handleOrder.size() > 256) {
            m_voices.erase(m_handleOrder.front());
            m_handleOrder.pop_front();
        }
        return h;
    }

    // Sounds are created ONCE per path and reused. The old code re-decoded the
    // same file on every shot.
    EngineSoundId acquire(const char* path) {
        const std::string full = resolve(path);
        if (auto it = m_sounds.find(full); it != m_sounds.end()) return it->second;

        std::error_code ec;
        const auto size = std::filesystem::file_size(full, ec);
        EngineSoundId id = ENGINE_AUDIO_NO_SOUND;

        if (!ec && size >= kStreamThreshold) {
            id = openStream(full);
        } else {
            std::vector<unsigned char> bytes;
            if (!readAll(full, bytes)) {
                LOG_ERROR("Audio", "failed to read '%s'", full.c_str());
                return ENGINE_AUDIO_NO_SOUND;
            }
            // flags 0 = fully decoded, so the provider owns the samples and this
            // buffer may die at the end of this scope. That is exactly what the
            // ABI promises, and why `bytes` is a local.
            const EngineAudioResult r = m_provider->createSound(
                m_self, bytes.data(), (uint64_t)bytes.size(), 0, full.c_str(), &id);
            if (r != ENGINE_AUDIO_OK) {
                LOG_ERROR("Audio", "provider rejected '%s' (%d)", full.c_str(), r);
                return ENGINE_AUDIO_NO_SOUND;
            }
        }
        if (id != ENGINE_AUDIO_NO_SOUND) m_sounds.emplace(full, id);
        return id;
    }

    // ── Pull streaming ──────────────────────────────────────────────────────
    // The engine serves reads from the file; the provider pulls on its own
    // worker. Nothing large is ever resident, and unlike the old path this does
    // not require the provider to know what a filesystem is.
    struct StreamFile {
        std::FILE* fp = nullptr;
        uint64_t   size = 0;
    };

    EngineSoundId openStream(const std::string& full) {
        auto sf = std::make_unique<StreamFile>();
        sf->fp = std::fopen(full.c_str(), "rb");
        if (!sf->fp) { LOG_ERROR("Audio", "failed to open '%s'", full.c_str()); return ENGINE_AUDIO_NO_SOUND; }
        std::error_code ec;
        sf->size = (uint64_t)std::filesystem::file_size(full, ec);

        EngineAudioStreamSource src{};
        src.structSize = (uint32_t)sizeof(src);
        src.totalBytes = sf->size;
        src.userData   = sf.get();
        src.read = [](void* ud, uint64_t offset, void* dst, uint64_t count) -> int64_t {
            auto* f = static_cast<StreamFile*>(ud);
            if (!f || !f->fp) return -1;
            // Absolute seek every read: the provider may loop or scrub, and the
            // ABI's read is offset-based precisely so it can.
            if (std::fseek(f->fp, (long)offset, SEEK_SET) != 0) return -1;
            return (int64_t)std::fread(dst, 1, (size_t)count, f->fp);
        };

        EngineSoundId id = ENGINE_AUDIO_NO_SOUND;
        const EngineAudioResult r = m_provider->createStream(
            m_self, &src, ENGINE_AUDIO_F_STREAM, full.c_str(), &id);
        if (r != ENGINE_AUDIO_OK) {
            // A provider that cannot pull is conformant; fall back to resident
            // bytes rather than losing the sound.
            std::fclose(sf->fp);
            LOG_WARN("Audio", "provider declined to stream '%s' (%d) — falling back",
                     full.c_str(), r);
            std::vector<unsigned char> bytes;
            if (!readAll(full, bytes)) return ENGINE_AUDIO_NO_SOUND;
            // F_STREAM here means the provider reads this buffer for the sound's
            // whole life, so the engine must keep it alive until destroySound.
            auto& kept = m_residentBytes[full];
            kept = std::move(bytes);
            if (m_provider->createSound(m_self, kept.data(), (uint64_t)kept.size(),
                                        ENGINE_AUDIO_F_STREAM, full.c_str(), &id)
                != ENGINE_AUDIO_OK) {
                m_residentBytes.erase(full);
                return ENGINE_AUDIO_NO_SOUND;
            }
            return id;
        }
        m_streams.emplace(full, std::move(sf));
        return id;
    }

    static bool readAll(const std::string& path, std::vector<unsigned char>& out) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n <= 0) { std::fclose(f); return false; }
        out.resize((size_t)n);
        const size_t got = std::fread(out.data(), 1, (size_t)n, f);
        std::fclose(f);
        out.resize(got);
        return got > 0;
    }

    void stopAll() {
        if (!live()) { m_voices.clear(); m_handleOrder.clear(); return; }
        for (auto& [h, v] : m_voices) m_provider->stop(m_self, v, 0);
        m_voices.clear();
        m_handleOrder.clear();
    }

    void reset() {
        for (auto& [path, sf] : m_streams) if (sf && sf->fp) std::fclose(sf->fp);
        m_streams.clear();
        m_residentBytes.clear();
        m_sounds.clear();
        m_voices.clear();
        m_handleOrder.clear();
        m_self = nullptr;
        m_ready.store(false, std::memory_order_release);
    }

    std::string resolve(const char* path) const {
        namespace fs = std::filesystem;
        fs::path p(path);
        if (p.is_absolute()) return p.string();
        if (!m_assetsRoot.empty()) return (m_assetsRoot / p).string();
        return std::string(path);
    }

    const EngineAudioProviderV1* m_provider = nullptr;
    EngineAudioHostServices      m_host{};
    void*                        m_self = nullptr;
    // Written by the device-open job, read by the game thread every play().
    std::atomic<bool>            m_ready{false};
    jobs::JobHandle              m_openJob;
    std::filesystem::path        m_assetsRoot;

    std::unordered_map<std::string, EngineSoundId>  m_sounds;
    std::unordered_map<std::string, std::unique_ptr<StreamFile>> m_streams;
    // Only for the fallback path: bytes a streaming provider reads for the
    // sound's lifetime, which the ABI makes the engine's job to keep alive.
    std::unordered_map<std::string, std::vector<unsigned char>> m_residentBytes;
    std::unordered_map<uint32_t, EngineVoiceId>     m_voices;
    std::deque<uint32_t>                            m_handleOrder;
    uint32_t                                        m_nextHandle = 0;
};
