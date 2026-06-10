// minimal_game — the smallest possible game on the engine SDK.
//
// Proves the API freeze: no editor code, no ImGui, no manual wiring. A
// window opens showing the default cube scene rendered through a Camera
// entity, with simulation running from boot.
#include <engine/engine.h>

int main() {
    EngineRuntime engine;
    if (!engine.init({})) return 1; // auto-detect project, GLFW window

    // Gameplay lives directly on the ECS world — the engine doesn't wrap it.
    flecs::world& world = engine.ctx().ecs;

    // Primary camera: above and behind the default cube grid, pitched down.
    Transform camT;
    camT.position = {0.0f, 6.0f, 16.0f};
    camT.rotation = bx::fromAxisAngle({1.0f, 0.0f, 0.0f}, -0.25f);
    world.entity("MainCamera")
        .set<Transform>(camT)
        .set<Camera>({}); // defaults: perspective, isPrimary=true

    // Sun pitched down ~50 degrees (lights emit along local -Z).
    Transform sunT;
    sunT.rotation = bx::fromAxisAngle({1.0f, 0.0f, 0.0f}, -0.9f);
    world.entity("Sun")
        .set<Transform>(sunT)
        .set<Light>({}); // defaults: directional, white

    engine.startSimulation(); // boot = play (in-place, no snapshot)

    engine.run([&](float dt) {
        engine.tick(dt); // systems + sim + primary-camera render to screen
    });

    engine.shutdown();
    return 0;
}
