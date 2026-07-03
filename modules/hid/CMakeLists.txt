cmake_minimum_required(VERSION 3.16)
project(hid VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# One backend TU per platform — the header (include/hid/hid.h) is the whole
# contract; adding a backend never touches consumers.
if(APPLE)
    set(HID_BACKEND src/hid_iohid.cpp)
else()
    # Windows Raw Input / Linux evdev backends land with their ports.
    set(HID_BACKEND src/hid_null.cpp)
endif()

add_library(hid STATIC ${HID_BACKEND})
add_library(hid::hid ALIAS hid)

target_include_directories(hid PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(APPLE)
    target_link_libraries(hid PUBLIC
        "-framework IOKit"
        "-framework CoreFoundation"
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(hid PUBLIC Threads::Threads)

# ── hid_dump — standalone verifier (list devices, stream raw events) ────────
add_executable(hid_dump tools/hid_dump.cpp)
target_link_libraries(hid_dump PRIVATE hid)

# ── ring_test — SPSC ring + clock correctness, no hardware/permission needed ─
add_executable(hid_ring_test tests/ring_test.cpp)
target_link_libraries(hid_ring_test PRIVATE hid)
