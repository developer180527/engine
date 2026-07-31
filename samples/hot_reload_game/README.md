---
status: unreviewed
---
# Hot-reloadable game module

C++ hot reload via the game-DLL pattern: your game compiles to a shared
library, `engine_host` runs the project and reloads the library live whenever
it changes. **World state survives** — entities, components, physics bodies
and assets all live in the host; only the code swaps.

## Try it

```sh
# terminal 1 — run the host on any project
build/engine_host /path/to/MyProject build/libhot_reload_game.so

# terminal 2 — edit kSpinSpeed / kBuildTag in game.cpp, then:
cmake --build build --target hot_reload_game
# the running game picks it up in ~1 second
```

## Reload cycle

```
old code:  onSimulationStop → onDetach → destroy → dlclose
new code:  dlopen (a temp copy) → create → onAttach → onSimulationStart
                                                       (same world!)
```

## Writing your own module

```cpp
#include <engine/game_module.h>

class MyGame final : public IEnginePlugin { /* lifecycle + phases */ };
ENGINE_GAME_MODULE(MyGame)
```

CMake — link **headers only**, never the engine libraries:

```cmake
add_library(my_game MODULE game.cpp)
target_link_libraries(my_game PRIVATE engine::headers)
if(APPLE)
    target_link_options(my_game PRIVATE -undefined dynamic_lookup)
endif()
```

Engine symbols resolve from the host process at load time. Linking
`engine_runtime` into a module would duplicate engine state — never do it.

## Rules (the price of hot reload)

1. **State lives in components, logic lives in the module.** Module member
   variables are rebuilt by `onSimulationStart` after every reload.
2. **Release everything that points into the module** (flecs systems,
   callbacks) in `onDetach` / `onSimulationStop` — dangling pointers after
   `dlclose` are instant crashes. Transient per-frame queries are the safe
   default.
3. **Component layout changes require a host restart** — the structs are
   shared with host-side code.
4. **Engine services go through the C API** (`#include <engine/engine_api.h>`:
   `engineKeyDown`, `engineEntityFind`, `enginePlaySound`, ...). Those calls
   execute host-side. Header-inline singletons (`Input::`,
   `InputSystem::get()`) duplicate across the dylib boundary — never use
   them in a module.
