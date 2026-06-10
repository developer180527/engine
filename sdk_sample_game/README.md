# SDK Sample Game

A complete fly-around scene built only from the engine SDK — no editor, no
project files, no external assets. Everything in the scene is a generated
primitive (cube / sphere / plane) plus lights.

## What it exercises

- Engine init + OS window (`EngineRuntime` + `GlfwPlatform`)
- Input: named axes (`Input::getAxis`) + raw mouse delta + cursor capture
- `PrimitiveLibrary` meshes — no files on disk
- Lights: directional sun, two colored point lights, one spot light
- The Camera-entity render path: `engine.tick(dt)` finds the primary
  `Camera` and renders straight to the backbuffer
- Simulation lifecycle (`startSimulation` at boot)

## Build & run

Built as part of the engine build:

```sh
cmake --build build
build/sdk_sample
```

## Controls

| Input        | Action                          |
|--------------|---------------------------------|
| `W A S D`    | fly forward / left / back / right |
| `Q / E`      | down / up                       |
| `Shift`      | fast                            |
| mouse        | look                            |
| `Esc`        | toggle mouse capture            |
| close window | quit                            |
