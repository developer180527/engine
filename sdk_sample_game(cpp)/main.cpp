// ── SDK sample game ──────────────────────────────────────────────────────────
// A complete fly-around scene built ONLY from the engine SDK — no editor, no
// project, no external assets. Tests in one program:
//
//   • engine init + OS window         (EngineRuntime + GlfwPlatform)
//   • input: WASD/QE fly + mouse look (Input axes + raw mouse delta)
//   • primitives: cube/sphere/plane   (PrimitiveLibrary — generated, no files)
//   • lights: directional + point + spot
//   • Camera entity drives rendering  (engine.tick(dt) → backbuffer)
//
// Controls:  WASD move · QE down/up · Shift fast · mouse look
//            Esc toggles mouse capture · close window to quit
#include <engine/engine.h>
#include <engine/input.h>

#include <cmath>

// FPS-style fly camera state. Rotation is yaw-then-pitch (no roll):
//   q = qYaw ⊗ qPitch   — pitch applied first (local X), yaw second (world Y)
struct FlyCamera {
    bx::Vec3 position { 0.0f, 2.0f, 8.0f };
    float    yaw      = 0.0f;   // radians; + turns left
    float    pitch    = 0.0f;   // radians; + looks up

    bx::Quaternion rotation() const {
        const bx::Quaternion qYaw   = bx::fromAxisAngle({0, 1, 0}, yaw);
        const bx::Quaternion qPitch = bx::fromAxisAngle({1, 0, 0}, pitch);
        return bx::normalize(bx::mul(qYaw, qPitch));
    }
    // Forward/right derived analytically from the same yaw/pitch — guaranteed
    // to agree with the rendered view direction.
    bx::Vec3 forward() const {
        return { -std::cos(pitch) * std::sin(yaw),
                  std::sin(pitch),
                 -std::cos(pitch) * std::cos(yaw) };
    }
    bx::Vec3 right() const {
        return { std::cos(yaw), 0.0f, -std::sin(yaw) };
    }
};

static void buildScene(flecs::world& world, PrimitiveLibrary& prims) {
    const MeshHandle cube   = prims.byName("cube");
    const MeshHandle sphere = prims.byName("sphere");
    const MeshHandle plane  = prims.byName("plane");

    auto spawn = [&](const char* name, MeshHandle mesh,
                     bx::Vec3 pos, bx::Vec3 scale) {
        Transform t;
        t.position = pos;
        t.scale    = scale;
        return world.entity(name)
            .set<Transform>(t)
            .set<Name>({name})
            .set<MeshRenderer>({mesh});
    };

    // Ground + a small arrangement of primitives near the origin
    spawn("Ground",      plane,  {  0.0f, 0.0f,  0.0f }, { 12.0f, 1.0f, 12.0f });
    spawn("Cube",        cube,   { -2.0f, 0.5f,  0.0f }, { 1.0f, 1.0f, 1.0f });
    spawn("TallCube",    cube,   { -2.0f, 1.75f, 0.0f }, { 0.5f, 0.5f, 0.5f });
    spawn("Sphere",      sphere, {  0.0f, 0.75f, 0.0f }, { 1.5f, 1.5f, 1.5f });
    spawn("SmallSphere", sphere, {  2.0f, 0.4f,  1.0f }, { 0.8f, 0.8f, 0.8f });
    spawn("Slab",        cube,   {  2.5f, 0.25f, -1.5f }, { 2.0f, 0.5f, 1.0f });

    // ── Lights ──────────────────────────────────────────────────────────────
    // Sun: low-intensity directional fill, pitched down (lights emit -Z).
    {
        Transform t;
        t.position = { 0.0f, 8.0f, 0.0f };
        t.rotation = bx::fromAxisAngle({1, 0, 0}, -0.9f);
        Light sun;
        sun.type      = LightType::Directional;
        sun.intensity = 1.5f;
        world.entity("Sun").set<Transform>(t).set<Name>({"Sun"}).set<Light>(sun);
    }
    // Warm point light hovering between the cube and sphere.
    {
        Transform t;
        t.position = { -0.8f, 2.2f, 0.8f };
        Light l;
        l.type      = LightType::Point;
        l.color     = { 1.0f, 0.55f, 0.25f };
        l.intensity = 18.0f;
        l.range     = 7.0f;
        world.entity("WarmPoint").set<Transform>(t).set<Name>({"WarmPoint"}).set<Light>(l);
    }
    // Cool point light off to the side.
    {
        Transform t;
        t.position = { 3.0f, 1.6f, 1.8f };
        Light l;
        l.type      = LightType::Point;
        l.color     = { 0.3f, 0.55f, 1.0f };
        l.intensity = 12.0f;
        l.range     = 6.0f;
        world.entity("CoolPoint").set<Transform>(t).set<Name>({"CoolPoint"}).set<Light>(l);
    }
    // Spot light aimed down at the sphere.
    {
        Transform t;
        t.position = { 0.0f, 4.0f, 2.5f };
        t.rotation = bx::fromAxisAngle({1, 0, 0}, -1.0f);
        Light l;
        l.type      = LightType::Spot;
        l.color     = { 0.9f, 1.0f, 0.9f };
        l.intensity = 25.0f;
        l.range     = 10.0f;
        l.spotInner = 18.0f;
        l.spotOuter = 30.0f;
        world.entity("Spot").set<Transform>(t).set<Name>({"Spot"}).set<Light>(l);
    }
}

int main() {
    // Fully standalone: no project on disk, no asset database, no demo scene.
    EngineConfig cfg;
    cfg.title             = "SDK Sample Game";
    cfg.autoDetectProject = false;
    cfg.openAssetDatabase = false;
    cfg.defaultScene      = false;

    // The game keeps the GLFW window pointer for input + cursor capture.
    auto platform = std::make_unique<GlfwPlatform>();
    GlfwPlatform* glfwPlat = platform.get();

    EngineRuntime engine;
    if (!engine.init(cfg, std::move(platform))) return 1;
    GLFWwindow* window = glfwPlat->glfwWindow();

    // Input: GLFW-polled key/mouse state + named axis bindings.
    InputSystem::get().init(window);
    auto& map = InputMap::get();
    map.bindAxis  ("MoveForward", Key::W, Key::S);
    map.bindAxis  ("MoveRight",   Key::D, Key::A);
    map.bindAxis  ("MoveUp",      Key::E, Key::Q);
    map.bindAction("Sprint",      Key::LeftShift);

    flecs::world& world = engine.ctx().ecs;
    buildScene(world, engine.primitives());

    // The camera is an ECS entity — engine.tick(dt) finds the primary Camera
    // and renders the world through it to the backbuffer.
    Camera cam;
    cam.clearColor[0] = 0.05f; cam.clearColor[1] = 0.06f;
    cam.clearColor[2] = 0.09f; cam.clearColor[3] = 1.0f;
    flecs::entity camEntity = world.entity("Player")
        .set<Transform>({})
        .set<Name>({"Player"})
        .set<Camera>(cam);

    FlyCamera fly;
    bool mouseCaptured  = true;
    int  skipLookFrames = 1; // swallow the cursor-warp delta on capture
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);

    engine.startSimulation(); // boot = play (plugins would tick here)

    engine.run([&](float dt) {
        InputSystem::get().processEvents();

        // Esc releases the mouse (reach the close button / other windows);
        // clicking back into the window recaptures it.
        if (mouseCaptured && Input::isKeyPressed(Key::Escape)) {
            mouseCaptured = false;
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        } else if (!mouseCaptured && Input::isMousePressed(MouseButton::Left)) {
            mouseCaptured  = true;
            skipLookFrames = 1; // ignore the recenter jump
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        }

        // ── Mouse look ───────────────────────────────────────────────────
        if (mouseCaptured && skipLookFrames == 0) {
            constexpr float kSens = 0.0025f;
            fly.yaw   -= Input::mouseDeltaX() * kSens; // right drag → look right
            fly.pitch -= Input::mouseDeltaY() * kSens; // down drag  → look down
            if (fly.pitch >  1.55f) fly.pitch =  1.55f;
            if (fly.pitch < -1.55f) fly.pitch = -1.55f;
        }
        if (skipLookFrames > 0) --skipLookFrames;

        // ── WASD/QE fly movement ─────────────────────────────────────────
        {
            const float speed = Input::isActionDown("Sprint") ? 12.0f : 4.0f;
            const bx::Vec3 fwd   = fly.forward();
            const bx::Vec3 right = fly.right();
            bx::Vec3 move = { 0.0f, 0.0f, 0.0f };
            move = bx::add(move, bx::mul(fwd,   Input::getAxis("MoveForward")));
            move = bx::add(move, bx::mul(right, Input::getAxis("MoveRight")));
            move.y += Input::getAxis("MoveUp");
            if (bx::length(move) > 0.001f)
                fly.position = bx::add(fly.position,
                                       bx::mul(bx::normalize(move), speed * dt));
        }

        // Drive the camera entity from the fly state.
        Transform t;
        t.position = fly.position;
        t.rotation = fly.rotation();
        camEntity.set<Transform>(t);

        engine.tick(dt); // systems + sim + primary-camera render to screen
    });

    engine.shutdown();
    return 0;
}
