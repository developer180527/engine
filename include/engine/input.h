#pragma once
// Input: polled key/mouse state plus named action/axis bindings.
//
//   InputSystem::get().init(window);          // GLFW-backed platforms only
//   InputMap::get().bindAction("Jump", Key::Space);

#include "runtime/input/input.h"
#include "runtime/input/input_system.h"
#include "runtime/input/input_map.h"
