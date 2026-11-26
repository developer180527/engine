## BUG-0082 — The packed scene header did not compile with MSVC
- found:     2026-11-27
- status:    fixed
- class:     build
- where:     src/assets/packed_scene.h
- symptom:   every Windows build failed in each file that includes packed_scene.h, the scene service among them. Push CI has no Windows leg, so it would first have shown in the nightly. Found by review.
- cause:     the little-endian check was `static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)`. Those are GCC and Clang macros; MSVC defines neither, and in a C++ expression an undefined name is an error.
- pinned-by: src/assets/packed_scene.h
- lane:      build (the nightly's Windows legs)
- proof:     the check is now `std::endian::native == std::endian::little` (<bit>, C++20), which every compiler the engine builds with has. A scan of the WO-071..092 diff found no other GCC- or Clang-only construct.
