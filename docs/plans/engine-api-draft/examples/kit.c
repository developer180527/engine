/* ----DRAFT------ What a kit becomes. It compiles against the headers; nothing
 * links yet. The same source works linked in or loaded as a shared library. */
#include "../eng/engine.h"

typedef struct Health { float current, maximum; } Health;

static EngResult onWorldStart(EngWorld* world, void* state) {
    static const EngFieldDesc fields[] = {
        { "current", ENG_FIELD_F32, offsetof(Health, current), 1, 0 },
        { "maximum", ENG_FIELD_F32, offsetof(Health, maximum), 1, 0 },
    };
    EngComponentDesc desc = ENG_INIT(EngComponentDesc);
    EngComponent health = 0;
    (void)state;
    desc.name = "combat.Health";
    desc.size = sizeof(Health);
    desc.align = _Alignof(Health);
    desc.fields = fields;
    desc.field_count = 2;
    return engRegisterComponent(world, &desc, &health);
}

const EngModuleDesc* combatKit(void);
const EngModuleDesc* combatKit(void) {
    static const EngModuleDesc module = {
        .struct_size = sizeof(EngModuleDesc),
        .api_version = ENG_API_VERSION,
        .name = "combat",
        .version = "1.0.0",
        .on_world_start = onWorldStart,
    };
    return &module;
}
