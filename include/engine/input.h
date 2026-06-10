#pragma once
// Input: polled key/mouse state plus named action/axis bindings.
//
//   InputSystem::get().init(window);          // GLFW-backed platforms only
//   InputMap::get().bindAction("Jump", Key::Space);

#include "runtime/input.h"
#include "runtime/input_system.h"
#include "runtime/input_map.h"
