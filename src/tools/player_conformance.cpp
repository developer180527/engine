// ── player_conformance — the conformance suites, inside a packaged player ────
// WO-093, docs/plans/provider-selection.md §8 (DR-0037).
//
// A shipped player links its providers statically, so a suite that loads a
// provider as a shared library has nothing to load. This runs the suite
// against the provider THIS binary linked: the code players get.
//
// Compiled only when ENGINE_PLAYER_CONFORMANCE is on, which engine_build turns
// on for --conformance and nothing else. A game's player never has it; the
// package shipped_player_check.py builds without it is checked for its absence.
//
//   engine_player --conformance audio            the audio suite (Rust, ffi/)
//   engine_player --conformance audio --mutate   the same, against a provider
//                                                broken on one rule: must FAIL
#include "tools/player_conformance.h"

#include <cstdio>
#include <string>

#include "engine/engine_audio_provider.h"
#include "engine_providers_built.h"   // ENGINE_PROVIDER_*: what this player linked

// tests/audio_conformance/ffi: engine_audio_conformance::SUITE, the list
// `cargo test` runs against a loaded module.
extern "C" int engine_audio_conformance_run(const EngineAudioProviderV1* provider);
#if ENGINE_PROVIDER_AUDIO_MINIAUDIO
extern "C" const EngineAudioProviderV1* engineAudioProviderV1(void);   // audio/miniaudio_provider.cpp
#endif

namespace {

// The mutation: audio-provider.md says playback "returns NO_VOICE for an
// invalid sound". This one hands back a voice id instead, as a provider that
// forgot the check would. The suite must catch it.
EngineVoiceId (*g_realPlay)(void*, const EngineAudioPlayDesc*) = nullptr;
EngineVoiceId playClaimingAVoice(void* self, const EngineAudioPlayDesc* desc) {
    const EngineVoiceId v = g_realPlay(self, desc);
    return v == ENGINE_AUDIO_NO_VOICE ? EngineVoiceId(0xBAD) : v;
}

int audio(bool mutate) {
#if ENGINE_PROVIDER_AUDIO_MINIAUDIO
    const EngineAudioProviderV1* p = engineAudioProviderV1();
    EngineAudioProviderV1 broken;
    if (mutate) {
        broken = *p;
        g_realPlay = p->play;
        broken.play = &playClaimingAVoice;
        p = &broken;
    }
    std::printf("engine_player: audio conformance against the linked provider 'miniaudio'%s\n",
                mutate ? ", MUTATED (play claims a voice for an invalid sound)" : "");
    const int failed = engine_audio_conformance_run(p);
    if (failed == 0) {
        std::printf("engine_player: conformance audio: every check passed\n");
        return 0;
    }
    std::printf("engine_player: conformance audio: FAILED (%d)\n", failed);
    return 1;
#else
    (void)mutate; (void)&playClaimingAVoice;
    std::printf("engine_player: conformance audio: this player linked no audio provider with a "
                "suite (audio 'none' has nothing to hold to the contract)\n");
    return 0;
#endif
}

} // namespace

int runPlayerConformance(const std::string& kind, bool mutate) {
    if (kind == "audio") return audio(mutate);
    std::fprintf(stderr, "engine_player: no conformance suite for '%s' (suites: audio; physics and "
                         "the renderer get theirs with their contracts, WO-074, WO-075)\n", kind.c_str());
    return 2;
}
