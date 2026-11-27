# ── EngineProviders.cmake — each provider declared once, built where it can run ─
# WO-085, docs/plans/provider-selection.md §3 (DR-0037).
#
# A provider is declared by a manifest beside its source,
# <kind>-<name>.provider.json, and one CMake call:
#
#   engine_provider(MANIFEST <file> [SOURCES <src>...] [LINKS <lib>...]
#                   [INCLUDES <dir>...])
#
# The call reads the manifest and checks it. Then, per runtime target,
#
#   engine_providers_apply(<target> FLAVOUR client|server [WRITE_LIST <file>])
#
# decides, for the platform being built and the target's flavour, whether each
# provider is BUILT. A provider is built when its `platforms` allow list has this
# platform, its `flavours` has the target's, every fact in `needs` is true in
# this platform's table (cmake/platforms/<platform>.json), and every SDK in `sdk`
# is found. A built provider's sources are compiled into the target and its
# usage requirements (links, includes) come from an INTERFACE target,
# engine_provider_<kind>_<name>. A provider that is not built is recorded with
# the reason.
#
# NOT BUILT IS NORMAL. A project CHOOSING one that is not built is the error, and
# it is refused by name at startup (WO-076) and later at engine_build (WO-086).
# What is never allowed is the other two failures the research found: a
# platform list nothing reads (O3DE's gem.json), and a missing SDK quietly
# turning a provider off so the build passes (CryEngine, O3DE). Here a malformed
# manifest is a configure error, and every "not built" says why, on the
# configure output, in the generated header and in providers.json.
#
# Each target gets a generated header, engine_providers_built.h (a PRIVATE
# include of that target), with ENGINE_PROVIDER_<KIND>_<NAME> defined to 1 or 0
# for every declared provider, and the full list with reasons. The build's
# provider list (src/plugins/stock_providers.cpp) registers a provider only when
# its macro is 1, and refuses to compile if a macro is missing.

get_filename_component(ENGINE_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/.. ABSOLUTE)
set(ENGINE_PROVIDER_KINDS physics scripting audio renderer)
set(ENGINE_PROVIDER_FLAVOURS client server)

# ── The platform being built, and its table ─────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ENGINE_PLATFORM macos)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ENGINE_PLATFORM linux)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(ENGINE_PLATFORM windows)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(ENGINE_PLATFORM android)
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(ENGINE_PLATFORM ios)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
    set(ENGINE_PLATFORM web)
else()
    message(FATAL_ERROR "engine: no platform table for CMAKE_SYSTEM_NAME '${CMAKE_SYSTEM_NAME}' "
                        "(cmake/platforms/ has: macos, linux, windows, android, ios, web)")
endif()

set(_engine_platform_dir ${CMAKE_CURRENT_LIST_DIR}/platforms)
file(GLOB _engine_platform_tables ${_engine_platform_dir}/*.json)
set(ENGINE_KNOWN_PLATFORMS "")
foreach(_t ${_engine_platform_tables})
    get_filename_component(_n ${_t} NAME_WE)
    list(APPEND ENGINE_KNOWN_PLATFORMS ${_n})
endforeach()

file(READ ${_engine_platform_dir}/${ENGINE_PLATFORM}.json _engine_platform_json)
string(JSON _facts_n LENGTH "${_engine_platform_json}" facts)
set(ENGINE_PLATFORM_FACTS "")          # names of facts this table states
set(ENGINE_PLATFORM_TRUE_FACTS "")     # the ones that are true here
math(EXPR _last "${_facts_n} - 1")
foreach(_i RANGE ${_last})
    string(JSON _fact MEMBER "${_engine_platform_json}" facts ${_i})
    string(JSON _val GET "${_engine_platform_json}" facts ${_fact})
    list(APPEND ENGINE_PLATFORM_FACTS ${_fact})
    if(_val)
        list(APPEND ENGINE_PLATFORM_TRUE_FACTS ${_fact})
    endif()
endforeach()

# Its graphics backends, in its default order (WO-089, core/device_select.h).
set(ENGINE_PLATFORM_GPU_APIS "")
string(JSON _gpu_n ERROR_VARIABLE _gpu_err LENGTH "${_engine_platform_json}" gpu_apis)
if(NOT _gpu_err AND _gpu_n GREATER 0)
    math(EXPR _last "${_gpu_n} - 1")
    foreach(_i RANGE ${_last})
        string(JSON _api GET "${_engine_platform_json}" gpu_apis ${_i})
        list(APPEND ENGINE_PLATFORM_GPU_APIS ${_api})
    endforeach()
endif()

# Every table must state the same facts, or a `needs` could pass on one platform
# only because the fact was never written down there.
foreach(_t ${_engine_platform_tables})
    file(READ ${_t} _j)
    foreach(_fact ${ENGINE_PLATFORM_FACTS})
        string(JSON _v ERROR_VARIABLE _err GET "${_j}" facts ${_fact})
        if(_err)
            message(FATAL_ERROR "engine: ${_t} does not state the fact '${_fact}' "
                                "(every platform table states every fact)")
        endif()
    endforeach()
endforeach()

# ── Reading a manifest ──────────────────────────────────────────────────────
# _engine_json_list(<out> <json> <key>): a JSON array of strings as a CMake list.
function(_engine_json_list out json key manifest)
    string(JSON _type ERROR_VARIABLE _err TYPE "${json}" ${key})
    if(_err)
        set(${out} "" PARENT_SCOPE)
        set(${out}_MISSING TRUE PARENT_SCOPE)
        return()
    endif()
    if(NOT _type STREQUAL "ARRAY")
        message(FATAL_ERROR "engine: ${manifest}: '${key}' must be an array of names")
    endif()
    string(JSON _n LENGTH "${json}" ${key})
    set(_list "")
    if(_n GREATER 0)
        math(EXPR _last "${_n} - 1")
        foreach(_i RANGE ${_last})
            string(JSON _v GET "${json}" ${key} ${_i})
            list(APPEND _list ${_v})
        endforeach()
    endif()
    set(${out} "${_list}" PARENT_SCOPE)
    set(${out}_MISSING FALSE PARENT_SCOPE)
endfunction()

function(engine_provider)
    cmake_parse_arguments(P "" "MANIFEST" "SOURCES;LINKS;INCLUDES" ${ARGN})
    if(NOT P_MANIFEST)
        message(FATAL_ERROR "engine_provider: MANIFEST is required")
    endif()
    get_filename_component(_manifest ${P_MANIFEST} ABSOLUTE)
    if(NOT EXISTS ${_manifest})
        message(FATAL_ERROR "engine_provider: no manifest at ${_manifest}")
    endif()
    file(READ ${_manifest} _j)

    foreach(_key kind name contract)
        string(JSON _v ERROR_VARIABLE _err GET "${_j}" ${_key})
        if(_err OR _v STREQUAL "")
            message(FATAL_ERROR "engine: ${_manifest}: '${_key}' is required")
        endif()
        set(_${_key} ${_v})
    endforeach()
    if(NOT _kind IN_LIST ENGINE_PROVIDER_KINDS)
        message(FATAL_ERROR "engine: ${_manifest}: unknown kind '${_kind}' "
                            "(kinds: ${ENGINE_PROVIDER_KINDS})")
    endif()
    get_filename_component(_file ${_manifest} NAME)
    if(NOT _file STREQUAL "${_kind}-${_name}.provider.json")
        message(FATAL_ERROR "engine: ${_manifest} declares ${_kind} '${_name}'; "
                            "the file must be named ${_kind}-${_name}.provider.json")
    endif()
    if(NOT EXISTS ${ENGINE_ROOT_DIR}/docs/contracts/${_contract}.md)
        message(FATAL_ERROR "engine: ${_manifest}: contract '${_contract}' has no "
                            "docs/contracts/${_contract}.md")
    endif()

    _engine_json_list(_platforms "${_j}" platforms ${_manifest})
    if(_platforms_MISSING OR NOT _platforms)
        message(FATAL_ERROR "engine: ${_manifest}: 'platforms' is required: the platforms "
                            "this provider supports (an allow list; there are no deny lists)")
    endif()
    foreach(_p ${_platforms})
        if(NOT _p IN_LIST ENGINE_KNOWN_PLATFORMS)
            message(FATAL_ERROR "engine: ${_manifest}: unknown platform '${_p}' "
                                "(cmake/platforms/ has: ${ENGINE_KNOWN_PLATFORMS})")
        endif()
    endforeach()
    _engine_json_list(_flavours "${_j}" flavours ${_manifest})
    if(_flavours_MISSING)
        set(_flavours ${ENGINE_PROVIDER_FLAVOURS})
    endif()
    foreach(_f ${_flavours})
        if(NOT _f IN_LIST ENGINE_PROVIDER_FLAVOURS)
            message(FATAL_ERROR "engine: ${_manifest}: unknown flavour '${_f}' "
                                "(flavours: ${ENGINE_PROVIDER_FLAVOURS})")
        endif()
    endforeach()
    _engine_json_list(_needs "${_j}" needs ${_manifest})
    foreach(_n ${_needs})
        if(NOT _n IN_LIST ENGINE_PLATFORM_FACTS)
            message(FATAL_ERROR "engine: ${_manifest}: needs '${_n}', which is not a platform "
                                "fact (facts: ${ENGINE_PLATFORM_FACTS})")
        endif()
    endforeach()
    _engine_json_list(_sdk "${_j}" sdk ${_manifest})
    _engine_json_list(_requires "${_j}" requires ${_manifest})
    _engine_json_list(_provides "${_j}" provides ${_manifest})
    _engine_json_list(_caps "${_j}" capabilities ${_manifest})
    # A renderer's device capabilities (WO-089): "device": { "needs", "fallbacks" }.
    # The names are checked by the resolver's reader (declFromManifest).
    set(_device_needs "")
    set(_device_fallbacks "")
    string(JSON _dev ERROR_VARIABLE _dev_err GET "${_j}" device)
    if(NOT _dev_err)
        _engine_json_list(_device_needs "${_dev}" needs ${_manifest})
        _engine_json_list(_device_fallbacks "${_dev}" fallbacks ${_manifest})
    endif()

    # Already declared?
    get_property(_declared GLOBAL PROPERTY ENGINE_PROVIDERS)
    set(_id ${_kind}:${_name})
    if(_id IN_LIST _declared)
        message(FATAL_ERROR "engine: ${_kind} provider '${_name}' is declared twice")
    endif()
    string(TOUPPER "${_kind}_${_name}" _macro)
    string(REPLACE "-" "_" _macro "${_macro}")
    string(REPLACE "." "_" _macro "${_macro}")
    set(_target engine_provider_${_kind}_${_name})
    string(REPLACE "-" "_" _target "${_target}")

    # Platform and SDK decide whether it CAN be built here; the flavour is
    # judged per target in engine_providers_apply.
    set(_why "")
    if(NOT ENGINE_PLATFORM IN_LIST _platforms)
        set(_why "it supports ${_platforms}, not ${ENGINE_PLATFORM}")
    endif()
    if(NOT _why)
        foreach(_n ${_needs})
            if(NOT _n IN_LIST ENGINE_PLATFORM_TRUE_FACTS)
                set(_why "it needs '${_n}', which ${ENGINE_PLATFORM} does not have")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _why)
        foreach(_s ${_sdk})
            if(NOT DEFINED ${_s} OR NOT EXISTS "${${_s}}")
                set(_why "the SDK ${_s} was not found (configure with -D${_s}=<path>)")
                break()
            endif()
        endforeach()
    endif()

    if(NOT _why)
        add_library(${_target} INTERFACE)
        if(P_LINKS)
            target_link_libraries(${_target} INTERFACE ${P_LINKS})
        endif()
        if(P_INCLUDES)
            target_include_directories(${_target} INTERFACE ${P_INCLUDES})
        endif()
    endif()

    set_property(GLOBAL APPEND PROPERTY ENGINE_PROVIDERS ${_id})
    foreach(_v kind name contract macro target why manifest)
        set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_${_v} "${_${_v}}")
    endforeach()
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_flavours "${_flavours}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_requires "${_requires}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_provides "${_provides}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_platforms "${_platforms}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_needs "${_needs}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_capabilities "${_caps}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_device_needs "${_device_needs}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_device_fallbacks "${_device_fallbacks}")
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_links "${P_LINKS}")
    set(_srcs "")
    foreach(_s ${P_SOURCES})
        get_filename_component(_a ${_s} ABSOLUTE)
        list(APPEND _srcs ${_a})
    endforeach()
    set_property(GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_sources "${_srcs}")
endfunction()

# ── A game's fixed selection (engine_build, WO-086) ──────────────────────────
# engine_build resolves the project's choice for the target and configures the
# shipping tree with it, e.g. "physics=jolt;scripting=lua;audio=miniaudio;
# renderer=forward-bgfx". Then only those are built, the player registers only
# those, and its runtime runs no resolver (ENGINE_PROVIDERS_FIXED). Empty in a
# development tree: every provider that can be built is, and the runtime
# resolves the project's choice at startup.
set(ENGINE_PROVIDERS_SELECTED "" CACHE STRING
    "kind=name;... chosen by engine_build for a shipped game; empty in a dev tree")

function(_engine_csv out list)
    string(REPLACE ";" "," _c "${list}")
    set(${out} "${_c}" PARENT_SCOPE)
endfunction()

# ── Applying the decisions to one runtime target ────────────────────────────
function(engine_providers_apply target)
    cmake_parse_arguments(A "" "FLAVOUR;WRITE_LIST" "" ${ARGN})
    if(NOT A_FLAVOUR IN_LIST ENGINE_PROVIDER_FLAVOURS)
        message(FATAL_ERROR "engine_providers_apply(${target}): FLAVOUR must be one of "
                            "${ENGINE_PROVIDER_FLAVOURS}")
    endif()
    get_property(_declared GLOBAL PROPERTY ENGINE_PROVIDERS)

    # The fixed selection, checked: every kind once, every name declared.
    set(_fixed 0)
    set(_selected_ids "")
    set(_chosen_hdr "")
    if(ENGINE_PROVIDERS_SELECTED)
        set(_fixed 1)
        foreach(_pair ${ENGINE_PROVIDERS_SELECTED})
            string(REPLACE "=" ";" _kv "${_pair}")
            list(LENGTH _kv _len)
            if(NOT _len EQUAL 2)
                message(FATAL_ERROR "ENGINE_PROVIDERS_SELECTED: '${_pair}' is not kind=name")
            endif()
            list(GET _kv 0 _k)
            list(GET _kv 1 _n)
            if(NOT "${_k}:${_n}" IN_LIST _declared)
                message(FATAL_ERROR "ENGINE_PROVIDERS_SELECTED: no ${_k} provider named '${_n}' is declared")
            endif()
            list(APPEND _selected_ids "${_k}:${_n}")
            string(TOUPPER "${_k}" _K)
            string(APPEND _chosen_hdr "#define ENGINE_PROVIDERS_CHOSEN_${_K} \"${_n}\"\n")
        endforeach()
        foreach(_k ${ENGINE_PROVIDER_KINDS})
            set(_count 0)
            foreach(_id ${_selected_ids})
                if(_id MATCHES "^${_k}:")
                    math(EXPR _count "${_count} + 1")
                endif()
            endforeach()
            if(NOT _count EQUAL 1)
                message(FATAL_ERROR "ENGINE_PROVIDERS_SELECTED must name exactly one ${_k} provider "
                                    "(it names ${_count})")
            endif()
        endforeach()
    endif()

    # Providers never link each other (R5): a provider's LINKS naming another
    # provider's target is refused, whichever was declared first.
    set(_provider_targets "")
    foreach(_id ${_declared})
        get_property(_t GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_target)
        list(APPEND _provider_targets ${_t})
    endforeach()

    set(_hdr "// Generated by cmake/EngineProviders.cmake for ${target} (${ENGINE_PLATFORM}, ${A_FLAVOUR}). Do not edit.\n")
    string(APPEND _hdr "#pragma once\n#define ENGINE_PROVIDERS_PLATFORM \"${ENGINE_PLATFORM}\"\n")
    string(APPEND _hdr "#define ENGINE_PROVIDERS_FLAVOUR \"${A_FLAVOUR}\"\n")
    string(APPEND _hdr "#define ENGINE_PROVIDERS_FIXED ${_fixed}\n${_chosen_hdr}")
    set(_facts "")
    foreach(_f ${ENGINE_PLATFORM_FACTS})
        if(_f IN_LIST ENGINE_PLATFORM_TRUE_FACTS)
            list(APPEND _facts "${_f}=1")
        else()
            list(APPEND _facts "${_f}=0")
        endif()
    endforeach()
    _engine_csv(_facts_csv "${_facts}")
    string(APPEND _hdr "#define ENGINE_PROVIDERS_PLATFORM_FACTS \"${_facts_csv}\"\n")
    _engine_csv(_known_csv "${ENGINE_KNOWN_PLATFORMS}")
    string(APPEND _hdr "#define ENGINE_PROVIDERS_KNOWN_PLATFORMS \"${_known_csv}\"\n")
    _engine_csv(_gpu_csv "${ENGINE_PLATFORM_GPU_APIS}")
    string(APPEND _hdr "#define ENGINE_PROVIDERS_GPU_APIS \"${_gpu_csv}\"\n")
    set(_rows "")
    set(_json "")
    foreach(_id ${_declared})
        foreach(_v kind name macro target why flavours links sources contract platforms needs requires provides device_needs device_fallbacks)
            get_property(_${_v} GLOBAL PROPERTY ENGINE_PROVIDER_${_id}_${_v})
        endforeach()
        foreach(_l ${_links})
            if(_l IN_LIST _provider_targets)
                message(FATAL_ERROR "engine: the ${_kind} provider '${_name}' links ${_l}, another "
                                    "provider. Providers depend on engine services, never on each "
                                    "other (provider-abi.md rule 9)")
            endif()
        endforeach()
        if(NOT _why AND NOT A_FLAVOUR IN_LIST _flavours)
            set(_why "it is not built for ${A_FLAVOUR} builds (flavours: ${_flavours})")
        endif()
        if(NOT _why AND _fixed AND NOT _id IN_LIST _selected_ids)
            set(_why "this game did not choose it (engine_build's selection)")
        endif()
        if(_why)
            set(_built 0)
            message(STATUS "provider ${_kind} ${_name}: not built for ${target}: ${_why}")
        else()
            set(_built 1)
            message(STATUS "provider ${_kind} ${_name}: built into ${target}")
            if(_sources)
                target_sources(${target} PRIVATE ${_sources})
            endif()
            target_link_libraries(${target} PUBLIC ${_target})
        endif()
        string(APPEND _hdr "#define ENGINE_PROVIDER_${_macro} ${_built}\n")
        string(REPLACE "\"" "'" _why_c "${_why}")
        foreach(_l platforms flavours needs requires provides device_needs device_fallbacks)
            _engine_csv(_${_l}_csv "${_${_l}}")
        endforeach()
        string(APPEND _rows "    { \"${_kind}\", \"${_name}\", ${_built}, \"${_why_c}\", \"${_contract}\", \"${_platforms_csv}\", \"${_flavours_csv}\", \"${_needs_csv}\", \"${_requires_csv}\", \"${_provides_csv}\", \"${_device_needs_csv}\", \"${_device_fallbacks_csv}\" }, \\\n")
        if(_json)
            string(APPEND _json ",\n")
        endif()
        string(APPEND _json "    { \"kind\": \"${_kind}\", \"name\": \"${_name}\", \"contract\": \"${_contract}\", \"built\": ${_built}, \"why_not\": \"${_why_c}\" }")
    endforeach()
    string(APPEND _hdr "// { kind, name, built, why not, contract, platforms, flavours, needs, requires, provides, device needs, device fallbacks }\n// for every declared provider; lists are comma-separated.\n")
    string(APPEND _hdr "#define ENGINE_PROVIDERS_DECLARED \\\n${_rows}\n")

    set(_dir ${CMAKE_BINARY_DIR}/generated/providers/${target})
    file(MAKE_DIRECTORY ${_dir})
    # Written only when it changes, so a reconfigure does not rebuild the world.
    file(CONFIGURE OUTPUT ${_dir}/engine_providers_built.h CONTENT "${_hdr}" @ONLY)
    target_include_directories(${target} PRIVATE ${_dir})
    if(A_WRITE_LIST)
        file(CONFIGURE OUTPUT ${A_WRITE_LIST}
             CONTENT "{\n  \"platform\": \"${ENGINE_PLATFORM}\",\n  \"flavour\": \"${A_FLAVOUR}\",\n  \"providers\": [\n${_json}\n  ]\n}\n" @ONLY)
    endif()
endfunction()
