/* ----DRAFT------ Shows the draft API on a hard case. It compiles against the
 * headers; nothing links yet.
 *
 * The Lunar Module on as many monitors as the player has: the view outside, the
 * main panel, and the DSKY. One simulation; one world; the panels are views. */
#include "../eng/engine.h"

typedef struct DescentEngine { float thrust_fraction, propellant_kg; } DescentEngine;

static void simulateDescentEngine(const EngSystemContext* context, const EngQueryBatch* batch) {
    DescentEngine* engines = (DescentEngine*)batch->columns[0];
    for (uint32_t i = 0; i < batch->count; ++i) {
        /* 14.8 kg/s at full thrust */
        engines[i].propellant_kg -= engines[i].thrust_fraction * 14.8f * (float)context->delta_seconds;
    }
}

static EngWindow openWindow(EngEngine* engine, const char* title, EngMonitor monitor, EngBool fullscreen) {
    EngWindowDesc desc = ENG_INIT(EngWindowDesc);
    EngWindow window = 0;
    desc.title = title;
    desc.monitor = monitor;
    desc.mode = fullscreen ? ENG_WINDOW_MODE_BORDERLESS_FULLSCREEN : ENG_WINDOW_MODE_WINDOWED;
    desc.flags = ENG_WINDOW_HANDLE_CLOSE; /* closing a panel must not end the mission */
    engCreateWindow(engine, &desc, &window);
    return window;
}

static EngEntity makeCamera(EngWorld* world, const char* name) {
    static const EngTransform identity = ENG_TRANSFORM_IDENTITY;
    EngCamera camera = { 1.0f, 0.1f, 0.0f, 0, 0.0f, {0, 0, 0} };
    EngEntity entity = 0;
    engCreateEntity(world, name, &entity);
    engAddComponent(world, entity, engFindComponent(world, ENG_COMPONENT_TRANSFORM), &identity);
    engAddComponent(world, entity, engFindComponent(world, ENG_COMPONENT_CAMERA), &camera);
    return entity;
}

static EngView showCamera(EngEngine* engine, EngWorld* world, EngEntity camera,
                          EngWindow window, EngViewRect rect, uint32_t fps) {
    EngViewDesc desc = ENG_INIT(EngViewDesc);
    EngView view = 0;
    desc.world = world;
    desc.camera = camera;
    desc.window = window;
    desc.rect = rect;
    desc.max_frames_per_second = fps;
    engCreateView(engine, &desc, &view);
    return view;
}

int main(void) {
    EngEngine* engine = NULL;
    EngWorld* world = NULL;
    EngEngineDesc engine_desc = ENG_INIT(EngEngineDesc);
    EngWorldDesc world_desc = ENG_INIT(EngWorldDesc);
    engine_desc.api_version = ENG_API_VERSION;
    engine_desc.app_name = "Eagle";
    if (engCreateEngine(&engine_desc, &engine) != ENG_OK) return 1;
    engCreateWorld(engine, &world_desc, &world);

    /* The simulation knows nothing about windows. */
    static const EngFieldDesc fields[] = {
        { "thrust_fraction", ENG_FIELD_F32, offsetof(DescentEngine, thrust_fraction), 1, 0 },
        { "propellant_kg",   ENG_FIELD_F32, offsetof(DescentEngine, propellant_kg),   1, 0 },
    };
    EngComponentDesc component_desc = ENG_INIT(EngComponentDesc);
    EngComponent descent_engine = 0;
    component_desc.name = "apollo.DescentEngine";
    component_desc.size = sizeof(DescentEngine);
    component_desc.align = _Alignof(DescentEngine);
    component_desc.fields = fields;
    component_desc.field_count = 2;
    engRegisterComponent(world, &component_desc, &descent_engine);

    EngQueryTerm term = { descent_engine, ENG_ACCESS_WRITE, {0, 0} };
    EngQueryDesc query_desc = ENG_INIT(EngQueryDesc);
    EngQuery query = 0;
    query_desc.terms = &term;
    query_desc.term_count = 1;
    engCreateQuery(world, &query_desc, &query);

    EngSystemDesc system_desc = ENG_INIT(EngSystemDesc);
    EngSystem system = 0;
    system_desc.name = "apollo.descent_engine";
    system_desc.phase = ENG_PHASE_FIXED_UPDATE;
    system_desc.query = query;
    system_desc.fn = simulateDescentEngine;
    engCreateSystem(world, &system_desc, &system);

    /* Windows and cameras: as many as the player's monitors allow. */
    EngEntity outside = makeCamera(world, "lmp_window_camera");
    EngEntity panel = makeCamera(world, "main_panel_camera");
    EngEntity dsky = makeCamera(world, "dsky_camera");

    EngWindowCapabilities caps = ENG_INIT(EngWindowCapabilities);
    EngMonitor monitors[8];
    uint32_t monitor_count = 8;
    const EngViewRect whole = { 0, 0, 0, 0 };
    engGetWindowCapabilities(engine, &caps);
    if (engGetMonitors(engine, monitors, &monitor_count) != ENG_OK) monitor_count = 0;

    if (caps.max_windows >= 3 && monitor_count >= 2) {
        EngWindow main_window = openWindow(engine, "Eagle: LMP window", monitors[0], ENG_TRUE);
        showCamera(engine, world, outside, main_window, whole, 0);
        showCamera(engine, world, panel,
                   openWindow(engine, "Eagle: main panel", monitors[1], ENG_TRUE), whole, 30);
        showCamera(engine, world, dsky,
                   openWindow(engine, "Eagle: DSKY", monitors[1], ENG_FALSE), whole, 15);
        engSetPrimaryWindow(engine, main_window); /* its display sets the rhythm */
    } else {
        /* One screen, one tab or one phone: the same cameras, one window. */
        const EngViewRect top = { 0, 0, 1.0f, 0.6f };
        const EngViewRect left = { 0, 0.6f, 0.7f, 0.4f };
        const EngViewRect right = { 0.7f, 0.6f, 0.3f, 0.4f };
        EngWindow window = openWindow(engine, "Eagle", 0, ENG_FALSE);
        showCamera(engine, world, outside, window, top, 0);
        showCamera(engine, world, panel, window, left, 30);
        showCamera(engine, world, dsky, window, right, 15);
    }

    /* The cockpit is not a keyboard: a real switch panel feeds an action. */
    EngAction abort_stage = 0;
    engRegisterAction(engine, "abort_stage", ENG_ACTION_BUTTON, &abort_stage);
    engBindAction(engine, abort_stage, "key:space", 1.0f);
    engBindAction(engine, abort_stage, "hid:16c0:05df/button1", 1.0f);

    /* Your loop. */
    while (engIsRunning(engine)) {
        EngEvent event = ENG_INIT(EngEvent);
        engPumpEvents(engine);
        while (engNextEvent(engine, &event)) {
            if (event.type == ENG_EVENT_WINDOW_CLOSE_REQUESTED) engShowWindow(engine, event.window, ENG_FALSE);
            if (event.type == ENG_EVENT_MONITOR_REMOVED) { /* move that panel into a remaining window */ }
        }
        if (engStep(engine, NULL) != ENG_OK) break;
    }
    engDestroyWorld(world);
    engDestroyEngine(engine);
    return 0;
}
