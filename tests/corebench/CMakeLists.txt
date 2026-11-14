# ── tests/corebench — the core benchmarks and fuzz tests, built on their own ──
#
# A standalone project, not part of the engine build: the job system and the
# memory manager in six engine sources plus TLSF, with no bgfx, Assimp, flecs or
# SQLite. It configures and builds in about a minute on any compiler, which is
# what lets .github/workflows/perf.yml measure on five desktop platforms and
# Android, and what the home server builds for its benchmark lane.
#
#   cmake -S tests/corebench -B build-corebench -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-corebench
#   build-corebench/jobs_bench --json bench.json
#
# One build of each: the job system has one backend since enkiTS was removed
# (2026-11-14, WO-079). Binaries are jobs_bench, jobs_fuzz_test, mem_test.
cmake_minimum_required(VERSION 3.21)
project(engine_perf LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
find_package(Threads REQUIRED)

if(MSVC)
    add_compile_options(/W3 /permissive- /Zc:__cplusplus /utf-8)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

add_library(perf_tlsf STATIC ${ROOT}/third_party/tlsf/tlsf.c)
target_include_directories(perf_tlsf PUBLIC ${ROOT}/third_party/tlsf)

set(CORE_SOURCES
    ${ROOT}/src/core/memory/mem.cpp
    ${ROOT}/src/core/mem_counters.cpp
    ${ROOT}/src/core/thread_qos.cpp
    ${ROOT}/src/core/jobs/jobs_shared.cpp)

add_library(perf_core STATIC ${CORE_SOURCES} ${ROOT}/src/core/jobs/jobs_native.cpp)
target_include_directories(perf_core PUBLIC ${ROOT}/src)
target_link_libraries(perf_core PUBLIC perf_tlsf Threads::Threads)
if(ANDROID)
    target_link_libraries(perf_core PUBLIC log)
endif()

foreach(t jobs_bench jobs_fuzz_test mem_test)
    add_executable(${t} ${ROOT}/tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE perf_core)
endforeach()
