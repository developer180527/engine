// ── keymap_test — hid_keymap tables must agree (audit M.7) ──────────────────
// Three hand-written tables translate keys: usageFromGlfw (window fallback
// events), nameFromGlfw (editor capture-a-key rebind), usageFromName
// (input.json specs). They drifted: nameFromGlfw stopped at F6/LAlt and
// usageFromName lacked RAlt/RSuper — captured keys silently produced dead
// bindings and hand-written "key:RAlt" silently failed. The round trip
//   usageFromGlfw(k) -> nameFromGlfw(k) -> usageFromName(name)
// must agree for EVERY key the forward map knows. Exits non-zero on first
// failure.
#include <cstdio>

#include "runtime/input/hid_keymap.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

int main() {
    std::printf("keymap_test: GLFW<->name<->usage round-trip gauntlet\n");

    // Every GLFW keycode usageFromGlfw resolves. Ranges mirror its own cases.
    int covered = 0, roundTripped = 0;
    auto probe = [&](int key) {
        const uint16_t direct = input::usageFromGlfw(key);
        if (!direct) return;                     // unmapped — nothing to pin
        ++covered;
        const char* name = input::nameFromGlfw(key);
        if (!name) {
            std::printf("  FAIL  nameFromGlfw(%d) null but usageFromGlfw=0x%02X\n",
                        key, direct);
            ++g_failures;
            return;
        }
        const uint16_t back = input::usageFromName(name);
        if (back != direct) {
            std::printf("  FAIL  key %d: '%s' round-trips 0x%02X != 0x%02X\n",
                        key, name, back, direct);
            ++g_failures;
            return;
        }
        ++roundTripped;
    };

    for (int k = 32;  k <= 96;  ++k) probe(k);   // printable block (A-Z, 0-9, space)
    for (int k = 256; k <= 348; ++k) probe(k);   // named keys, F-row, modifiers

    CHECK(g_failures == 0, "all %d mapped keys round-trip (%d verified)",
          covered, roundTripped);
    CHECK(covered >= 50, "coverage sanity: %d keys mapped (A-Z, 0-9, F1-F12, "
          "modifiers, nav)", covered);

    // The two M.7 gap classes, pinned explicitly:
    CHECK(input::usageFromName("RAlt")   == 0xE6, "'key:RAlt' resolves");
    CHECK(input::usageFromName("RSuper") == 0xE7, "'key:RSuper' resolves");
    CHECK(input::nameFromGlfw(301) && input::usageFromName(input::nameFromGlfw(301)) == 0x45,
          "F12 captures and round-trips");
    CHECK(input::nameFromGlfw(344) && input::usageFromName(input::nameFromGlfw(344)) == 0xE5,
          "RShift captures and round-trips");

    // Case-insensitive single letters (documented usageFromName behavior).
    CHECK(input::usageFromName("w") == input::usageFromName("W"),
          "single letters are case-insensitive");

    if (g_failures) { std::printf("keymap_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("keymap_test: ALL PASS\n");
    return 0;
}
