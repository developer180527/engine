/* ── audio_abi_check — the C half of the audio ABI freeze ────────────────────
 *
 * WHY THIS IS C, IN A REPO WHOSE TESTS ARE OTHERWISE RUST. It is not a
 * behavioural test — tests/audio_conformance owns that, and should keep owning
 * it. This asserts facts *about C*: what a C compiler computes for sizeof and
 * offsetof on the structs in engine_audio_provider.h. Rust cannot check that
 * from outside; asking it to would only compare its own transcription against
 * itself.
 *
 * WHY IT EXISTS AT ALL. The header carried ENGINE_AUDIO_FROZEN assertions and
 * a comment claiming "the C++ and C11 builds in this repo" checked them. They
 * did not: NO translation unit in the tree included the header, so every one of
 * those static asserts had never been compiled even once. The Rust suite pins
 * ITS OWN layout at compile time, which catches Rust-side drift and is blind to
 * C-side drift — so the C structs could have been reshaped freely and every
 * test in the repo would still have passed, right up until a real provider
 * loaded and read the wrong fields.
 *
 * Including the header is therefore most of the point of this file: it makes
 * the static asserts fire. The runtime body pins the two things a static assert
 * cannot reach — field OFFSETS (a reorder preserves every size) and the name
 * hash, which engine and provider must compute identically or every findSound
 * silently misses.
 *
 * The numbers here and in audio_conformance/src/lib.rs are the same numbers.
 * Neither side can now move alone.
 */
#include <engine/engine_audio_provider.h>

#include <stddef.h>
#include <stdio.h>

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){printf("  FAIL  " __VA_ARGS__);printf("\n");++g_failures;} \
                           else {printf("  ok    " __VA_ARGS__);printf("\n");} } while(0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("audio_abi_check: the C side of EngineAudioProviderV1\n");

    /* ── Sizes ───────────────────────────────────────────────────────────────
     * ENGINE_AUDIO_FROZEN already failed the build if these were wrong. Printed
     * here so a mismatch reports a number rather than a template error, and so
     * the values are visible next to the Rust ones when they disagree. */
    CHECK(sizeof(EngineAudioDeviceDesc)   ==  16, "EngineAudioDeviceDesc   16 (%zu)", sizeof(EngineAudioDeviceDesc));
    CHECK(sizeof(EngineAudioListener)     ==  52, "EngineAudioListener     52 (%zu)", sizeof(EngineAudioListener));
    CHECK(sizeof(EngineAudioEmitterUpdate)==  48, "EngineAudioEmitterUpdate 48 (%zu)", sizeof(EngineAudioEmitterUpdate));
    CHECK(sizeof(EngineAudioPlayDesc)     ==  64, "EngineAudioPlayDesc     64 (%zu)", sizeof(EngineAudioPlayDesc));
    CHECK(sizeof(EngineAcousticGeometry)  ==  48, "EngineAcousticGeometry  48 (%zu)", sizeof(EngineAcousticGeometry));
    CHECK(sizeof(EngineAudioStreamSource) ==  32, "EngineAudioStreamSource 32 (%zu)", sizeof(EngineAudioStreamSource));
    CHECK(sizeof(EngineAudioHostServices) ==  56, "EngineAudioHostServices 56 (%zu)", sizeof(EngineAudioHostServices));
    CHECK(sizeof(EngineAudioStats)        ==  48, "EngineAudioStats        48 (%zu)", sizeof(EngineAudioStats));
    CHECK(sizeof(EngineAudioProviderV1)   == 120, "EngineAudioProviderV1  120 (%zu)", sizeof(EngineAudioProviderV1));

    /* ── Offsets ─────────────────────────────────────────────────────────────
     * What sizeof cannot see. Swapping two same-width fields, or two function
     * pointers in the table, keeps every size identical and silently rebinds
     * every call — the exact failure engine_api_table.h froze offsets to stop.
     * Spot-checked at the places a reorder shows up first: the head, the field
     * appended most recently, and the pointers either side of the new ones. */
    CHECK(offsetof(EngineAudioStats, samplesPlayed) == 24,
          "Stats.samplesPlayed at 24 (%zu)", offsetof(EngineAudioStats, samplesPlayed));
    CHECK(offsetof(EngineAudioStats, hostTimeNs) == 40,
          "Stats.hostTimeNs at 40 (%zu) — it must pair with samplesPlayed, not "
          "drift into the padding after cpuLoad",
          offsetof(EngineAudioStats, hostTimeNs));
    CHECK(offsetof(EngineAudioProviderV1, create) == 8,
          "provider.create at 8 (%zu)", offsetof(EngineAudioProviderV1, create));
    CHECK(offsetof(EngineAudioProviderV1, destroySound) == 40,
          "provider.destroySound at 40 (%zu)", offsetof(EngineAudioProviderV1, destroySound));
    CHECK(offsetof(EngineAudioProviderV1, createStream) == 48,
          "provider.createStream at 48 (%zu)", offsetof(EngineAudioProviderV1, createStream));
    CHECK(offsetof(EngineAudioProviderV1, findSound) == 56,
          "provider.findSound at 56 (%zu)", offsetof(EngineAudioProviderV1, findSound));
    CHECK(offsetof(EngineAudioProviderV1, getStats) == 112,
          "provider.getStats at 112 (%zu) — the last entry, so it moves if "
          "ANYTHING above it changes width",
          offsetof(EngineAudioProviderV1, getStats));
    CHECK(offsetof(EngineAudioHostServices, alloc) == 16,
          "hostServices.alloc at 16 (%zu)", offsetof(EngineAudioHostServices, alloc));
    CHECK(offsetof(EngineAudioHostServices, nowNs) == 48,
          "hostServices.nowNs at 48 (%zu)", offsetof(EngineAudioHostServices, nowNs));

    /* ── The name hash ───────────────────────────────────────────────────────
     * The same literals tests/audio_conformance pins in Rust. If the two
     * implementations ever diverge, every findSound misses and NOTHING reports
     * an error — the sound simply never plays, which is close to undebuggable
     * from the outside. */
    CHECK(engineAudioHashName("Play_Gunshot") == 7375508369329266918ull,
          "hash(\"Play_Gunshot\") matches the Rust suite (%llu)",
          (unsigned long long)engineAudioHashName("Play_Gunshot"));
    CHECK(engineAudioHashName("") == 0xCBF29CE484222325ull,
          "the empty name is the unmodified FNV offset basis — a wrong seed "
          "shows up here first");
    CHECK(engineAudioHashName(NULL) == 0, "a null name hashes to 0, never a crash");
    CHECK(engineAudioHashName("a") != engineAudioHashName("b"),
          "distinct names do not collide trivially");

    if (g_failures) {
        printf("\naudio_abi_check: FAIL — %d\n", g_failures);
        return 1;
    }
    printf("\naudio_abi_check: PASS\n");
    return 0;
}
