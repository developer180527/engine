/* combat_module.c: the combat kit as a shared library, for project.json's
 * "kits" (WO-094). combat.c is unchanged: built with ENG_CLIENT, its eng*
 * calls go to the stubs below, filled from the engine's get_proc. Nothing of
 * the engine's is linked, and only engModuleEntry is exported. */
#define ENG_CLIENT_IMPLEMENTATION
#include <eng/eng_client.h>

#include "combat.h"

ENG_MODULE_EXPORT const EngModuleDesc* engModuleEntry(EngGetProcFn get_proc);
ENG_MODULE_EXPORT const EngModuleDesc* engModuleEntry(EngGetProcFn get_proc) {
    if (engClientLoad(get_proc, NULL) != ENG_OK) return NULL;
    return combatKit();
}
