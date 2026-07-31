# ── engine_net — Rust staticlib wired into the CMake build ───────────────────
#
# Cargo owns compilation; CMake owns linkage. The Rust crate is built by an
# external command and imported as a plain static library, so from the rest of
# the tree `engine_net` looks exactly like any other CMake target — nothing
# else in the build needs to know Rust is involved.
#
# The build is OPTIONAL by design (ENGINE_WITH_RUST). Rust is a real new
# dependency for every contributor and every CI leg, and the engine must still
# configure and build without it while the networking module is a skeleton.
# When the module becomes load-bearing this default is what changes — one line,
# deliberately, not by accident.

find_program(CARGO_EXECUTABLE cargo)

if(NOT CARGO_EXECUTABLE)
    message(STATUS "engine_net: cargo not found — Rust module DISABLED "
                   "(install rustup, or ignore: nothing depends on it yet)")
    return()
endif()

# Resolved at CONFIGURE time, not through generator expressions: a custom
# command's OUTPUT path becomes a literal filename in the ninja/make graph, so
# a $<CONFIG> there is never expanded and the build fails looking for a file
# with the genex text in its name.
#
# Multi-config generators (Xcode, MSVC) leave CMAKE_BUILD_TYPE empty and would
# need per-config IMPORTED_LOCATION_<CONFIG>; until this module is load-bearing
# they get the debug profile, which is correct-but-unoptimised rather than
# wrong. Revisit when Windows/MSVC actually builds the Rust module.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
    set(_cargo_flags "")
    set(_cargo_outdir "debug")
else()
    set(_cargo_flags "--release")
    set(_cargo_outdir "release")
endif()
set(_cargo_target_dir "${CMAKE_BINARY_DIR}/rust")

if(MSVC)
    set(_lib_name "engine_net.lib")
else()
    set(_lib_name "libengine_net.a")
endif()
set(_lib_path "${_cargo_target_dir}/${_cargo_outdir}/${_lib_name}")

# Cargo does its own dependency tracking and is fast when up to date, so this
# always runs and lets cargo decide. DEPENDS still lists the sources so a
# no-op cargo invocation is skipped entirely when nothing changed.
add_custom_command(
    OUTPUT  "${_lib_path}"
    COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${_cargo_target_dir}"
            ${CARGO_EXECUTABLE} build ${_cargo_flags}
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/lib.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    COMMENT "Building engine_net (Rust, ${_cargo_outdir})"
    VERBATIM
)
add_custom_target(engine_net_cargo DEPENDS "${_lib_path}")

add_library(engine_net STATIC IMPORTED GLOBAL)
add_dependencies(engine_net engine_net_cargo)
set_target_properties(engine_net PROPERTIES
    IMPORTED_LOCATION "${_lib_path}")
target_include_directories(engine_net INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# A Rust staticlib does NOT carry its std dependencies — the C++ link step has
# to supply them, and a missing one surfaces as a pile of undefined symbols
# rather than anything mentioning Rust. Listed per platform up front so that
# never becomes a debugging session.
if(APPLE)
    target_link_libraries(engine_net INTERFACE
        "-framework CoreFoundation" "-framework Security")
elseif(UNIX)
    target_link_libraries(engine_net INTERFACE pthread dl m)
elseif(WIN32)
    target_link_libraries(engine_net INTERFACE
        ws2_32 userenv ntdll bcrypt advapi32)
endif()

message(STATUS "engine_net: Rust module ENABLED (${CARGO_EXECUTABLE})")
