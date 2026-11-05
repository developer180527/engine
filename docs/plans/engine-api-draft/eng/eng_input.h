/* ----DRAFT------ Nothing in this file is promised, built or linked yet.
 *
 * eng_input.h: gameplay binds to actions, never to keys or devices. Raw keys and
 * pointers per window arrive as events (eng_window.h), for UI and text. */
#ifndef ENG_INPUT_H
#define ENG_INPUT_H
#include "eng_base.h"

ENG_EXTERN_C_BEGIN

typedef uint32_t EngActionType;
enum { ENG_ACTION_BUTTON = 0, ENG_ACTION_AXIS1 = 1, ENG_ACTION_AXIS2 = 2 };

/* Registering a name twice returns the same action. A project's input.json can
 * declare the same names, so bindings stay data when you want them to. */
ENG_API EngResult engRegisterAction(EngEngine* engine, const char* name,
                                    EngActionType type, EngAction* out);
/* binding: "key:space", "pad:south", "mouse:delta", "hid:046d:c216/button3". */
ENG_API EngResult engBindAction(EngEngine* engine, EngAction action,
                                const char* binding, float scale);

typedef struct EngActionState {
    uint32_t struct_size;  /* set by the caller */
    EngBool held, pressed, released;
    float x, y;
} EngActionState;
ENG_API EngResult engGetActionState(EngEngine* engine, EngAction action,
                                    EngActionState* out);

/* Feed an action from anything: a custom panel on a serial port, a network
 * cockpit, a test. It takes the same path as a device, so it replays too. */
ENG_API EngResult engPushActionValue(EngEngine* engine, EngAction action,
                                     float x, float y);

/* What simulation code reads, sampled once per fixed step or fed from a replay.
 * Live state (above) is for menus and cameras; this is for anything that must
 * replay. Bit N of the masks is the Nth action declared for intents. */
typedef struct EngIntent {
    uint32_t struct_size;  /* set by the caller */
    float move_x, move_y;
    float look_dx, look_dy;
    uint32_t held, pressed, released;
} EngIntent;
ENG_API EngResult engDeclareIntentAction(EngWorld* world, EngAction action, uint32_t* out_bit);
ENG_API EngResult engSetLocalController(EngWorld* world, EngEntity entity);
/* ENG_ERROR_NOT_FOUND when nothing supplied an intent for this entity this tick. */
ENG_API EngResult engGetIntent(EngWorld* world, EngEntity entity, EngIntent* out);

ENG_EXTERN_C_END
#endif /* ENG_INPUT_H */
