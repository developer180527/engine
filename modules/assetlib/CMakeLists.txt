cmake_minimum_required(VERSION 3.16)
project(assetlib VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sources ───────────────────────────────────────────────────────────────────
add_library(assetlib STATIC
    src/uuid.cpp
    src/asset_registry.cpp
    src/mesh_asset.cpp
    src/texture_asset.cpp
    src/scene_asset.cpp
    src/cook_pipeline.cpp
    src/dependency_graph.cpp
    src/file_watcher.cpp
    src/ddc.cpp
    src/task_graph.cpp
)

# ── BLAKE3 (vendored, C) ─────────────────────────────────────────────────────
# Content hash for the DDC — cryptographic-strength keys are what make a
# SHARED cache safe (an FNV collision on a shared store silently serves the
# wrong bytes to the whole studio). Portable kernel everywhere; NEON on ARM.
set(BLAKE3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/blake3)
set(BLAKE3_SRCS
    ${BLAKE3_DIR}/blake3.c
    ${BLAKE3_DIR}/blake3_dispatch.c
    ${BLAKE3_DIR}/blake3_portable.c
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    # NEON kernel autodetects via blake3_impl.h on AArch64.
    list(APPEND BLAKE3_SRCS ${BLAKE3_DIR}/blake3_neon.c)
else()
    # x86 SIMD kernels not vendored — portable only (correct, just slower).
    # Defines must be identical on EVERY blake3 TU: MAX_SIMD_DEGREE is derived
    # from them in blake3_impl.h and sizes buffers shared across the TUs.
    set_source_files_properties(${BLAKE3_SRCS} PROPERTIES COMPILE_DEFINITIONS
        "BLAKE3_NO_SSE2;BLAKE3_NO_SSE41;BLAKE3_NO_AVX2;BLAKE3_NO_AVX512")
endif()
target_sources(assetlib PRIVATE ${BLAKE3_SRCS})
target_include_directories(assetlib PRIVATE ${BLAKE3_DIR})
# The hash runs over every source byte in the project on scan; at Debug -O0
# it throttles registry scans + DDC key computation. Vendored, deterministic,
# never debugged with the engine — keep it optimized in Debug builds.
if(CMAKE_C_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
    set_source_files_properties(${BLAKE3_SRCS} PROPERTIES COMPILE_OPTIONS "-O2")
endif()

add_library(assetlib::core ALIAS assetlib)

target_include_directories(assetlib
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── SQLite ────────────────────────────────────────────────────────────────────
# Use system sqlite3 — available on macOS, Linux, Windows via vcpkg
find_package(SQLite3 REQUIRED)
target_link_libraries(assetlib PUBLIC SQLite3::SQLite3)

# ── Platform ──────────────────────────────────────────────────────────────────
if(APPLE)
    target_link_libraries(assetlib PRIVATE "-framework CoreServices")
endif()

# ── Strict warnings, zero engine deps ────────────────────────────────────────
target_compile_options(assetlib PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall -Wextra -Wno-unused-parameter>
)

# ── Optional: tests ──────────────────────────────────────────────────────────
option(ASSETLIB_BUILD_TESTS "Build assetlib tests" OFF)
if(ASSETLIB_BUILD_TESTS)
    add_subdirectory(tests)
endif()
