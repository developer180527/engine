 the input subsystem as a whole (InputSystem, InputMap, Input, and InputEvent), the code is clean, lightweight, and refreshingly free of unnecessary abstraction, but there are several architectural issues that will likely surface as the engine matures.

The largest problem is that the input layer is not actually platform-agnostic despite appearing to be part of the runtime abstraction. Core input types (Key, MouseButton, and much of InputSystem) are directly tied to GLFW constants and headers, meaning GLFW has effectively leaked into the public engine API. This makes future platform implementations more difficult because key codes, button values, and input semantics are no longer owned by the engine but inherited from a specific windowing library. If GLFW is ever replaced, embedded, or supplemented by another backend, compatibility pressure immediately appears throughout gameplay code.

Another issue is the heavy reliance on global singletons. Both InputSystem::get() and InputMap::get() are globally accessible, meaning any subsystem, plugin, gameplay component, or future scripting system can reach input state from anywhere. While convenient, this creates hidden dependencies and makes ownership, initialization ordering, testing, replay systems, multiple worlds, and multiple runtime instances significantly harder. The runtime documentation already states that only one runtime should exist per process, and the input architecture reinforces that limitation.

There is also a growing mismatch between the event model and the polling model. InputEvent exists as a generic event representation, yet the actual implementation largely bypasses events and polls GLFW state directly. Scroll and text input use callbacks, while keyboard and mouse buttons use polling. This hybrid design is practical, but it means there are effectively two input systems operating simultaneously, and the InputEvent abstraction currently provides little value. If gamepad support, input recording, network replay, editor event routing, or input remapping become more sophisticated, the split architecture may become harder to evolve consistently.

The action and axis system is intentionally vector-based, which is perfectly reasonable for a small number of bindings, but every action query performs repeated linear searches through registered actions and then repeated scans through bound keys. Today this cost is trivial, but gameplay code often calls input queries many times per frame across many systems. As the number of actions grows, lookup cost scales linearly rather than remaining constant.

The input mapping layer is also entirely digital. Axes are synthesized from two keys:
Positive Key -> +1
Negative Key -> -1

which works well for keyboard movement but does not naturally extend to analog devices such as thumbsticks, triggers, steering wheels, flight sticks, pressure-sensitive inputs, or touch gestures. Supporting those later will likely require expanding or partially redesigning the current axis model.

Another subtle issue is callback ownership. InputSystem chains scroll and character callbacks by capturing the previously installed GLFW callbacks. This works well with ImGui today, but it implicitly assumes no other subsystem will attempt similar callback chaining. As more runtime features appear, callback ordering and ownership can become increasingly fragile.

The UI capture mechanism is also fairly coarse. The engine effectively gates entire categories of input through:
setUICapture(keyboard, mouse)

which is sufficient for current editor workflows but may become limiting for more advanced UI systems where partial input sharing, event bubbling, focus hierarchies, viewport-specific routing, or editor/game simultaneous input are required.

Finally, the entire subsystem assumes a single window, single cursor, single input context, and single runtime. Mouse state, cursor position, key state arrays, text input buffers, callback registration, and UI capture flags all live in a single global state container. This dramatically simplifies implementation but would become problematic if the engine ever supports multiple windows, multiple editor viewports, remote play sessions, embedded runtimes, or split execution contexts.

Overall, the subsystem is efficient and pragmatic for a game engine at its current stage, but its main architectural weaknesses are GLFW leakage into public APIs, singleton-driven global state, a partially unused event abstraction, linear action lookup, limited analog-input support, callback ownership assumptions, coarse UI capture control, and an architecture fundamentally built around a single-window, single-runtime model. None of these are immediate correctness bugs, but they are the areas most likely to create friction as the engine grows beyond its current scope.
