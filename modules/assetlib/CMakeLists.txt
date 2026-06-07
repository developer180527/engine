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
)

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
