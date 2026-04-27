cmake_minimum_required(VERSION 3.20)
project(engine CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# bgfx
set(BGFX_BUILD_EXAMPLES OFF CACHE BOOL "")
set(BGFX_BUILD_TOOLS OFF CACHE BOOL "")
add_subdirectory(third_party/bgfx.cmake)

# GLFW
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "")
set(GLFW_BUILD_TESTS OFF CACHE BOOL "")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "")
add_subdirectory(third_party/glfw)

# Engine executable
add_executable(engine src/main.cpp)

target_link_libraries(engine PRIVATE
    bgfx
    bx
    bimg
    glfw
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework Metal"
    "-framework IOKit"
)

target_include_directories(engine PRIVATE src)
