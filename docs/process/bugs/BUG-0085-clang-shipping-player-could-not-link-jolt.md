## BUG-0085 — The Clang shipping player could not link Jolt
- found:     2026-11-27
- status:    fixed
- class:     build
- where:     CMakeLists.txt
- symptom:   on Linux x64 with Clang, `engine_build` failed building the shipping player: `libJolt.a: error adding symbols: file format not recognized`. GCC, AppleClang and MSVC linked it.
- cause:     Jolt turns on LTO for Release builds by default (its `INTERPROCEDURAL_OPTIMIZATION` option). With Clang its archive then holds LLVM bitcode, and Ubuntu's default linker, ld.bfd, has no LLVMgold plugin to read it. The development tree is Debug, so only the Release shipping tree hit it.
- pinned-by: scripts/shipped_player_check.py
- lane:      ci
- proof:     Jolt's LTO is forced off beside its other options; the CI shipped-player step packages a Release player on Linux Clang and links it.
