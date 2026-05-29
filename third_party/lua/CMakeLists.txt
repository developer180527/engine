# Static Lua 5.4 library for embedding. Vanilla Lua ships a Makefile only;
# this compiles the interpreter core + standard library as a static lib.
# lua.c (standalone REPL) and luac.c (bytecode compiler) each define main()
# and are excluded.
cmake_minimum_required(VERSION 3.16)
project(lua C)

file(GLOB LUA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM LUA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lua.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/luac.c")

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Platform feature defines (enables dlopen-based require, etc.)
if(APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
endif()

# Lua is C; C++ consumers include <lua.hpp> which wraps headers in extern "C".
