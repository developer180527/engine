/* combat.h: the combat kit's contract, in C (WO-073).
 *
 * Health and damage are not an engine primitive: every game models them
 * differently, so they live in a kit. This is that kit on the engine's C API,
 * the port of the C++ CombatKit (kits/CombatKit, its own repository), with the
 * same rule: damage goes into an inbox, the kit drains it once per simulation
 * step, subtracts it from Health, and tags the dead with Died.
 *
 * A dealer (a gun, a melee AI) includes only this header and calls
 * combatDealDamage. Kits cooperate through components, never each other's
 * code. Nothing here includes flecs or the engine's C++. */
#ifndef COMBAT_H
#define COMBAT_H
#include <eng/engine.h>

ENG_EXTERN_C_BEGIN

/* On anything that can be hurt. Only entities with Health take damage: damage
 * to anything else is dropped, so stray shots never make scenery mortal. */
#define COMBAT_HEALTH "combat.Health"
typedef struct CombatHealth { float current, max; } CombatHealth;   /* default 100 / 100 */

/* Raw damage waiting for the kit; removed when the kit has applied it. */
#define COMBAT_DAMAGE_INBOX "combat.DamageInbox"
typedef struct CombatDamageInbox { float pending; } CombatDamageInbox;

/* Added once, when Health reaches zero. The game decides what death looks
 * like (a clip, a ragdoll, loot) and destroys the entity when it is done. */
#define COMBAT_DIED "combat.Died"
typedef struct CombatDied { float at_sim_time; } CombatDied;

/* The module, for EngEngineDesc.modules. */
const EngModuleDesc* combatKit(void);

/* Adds raw damage to target's inbox. A dead entity, or an amount that is not
 * above zero, does nothing. ENG_ERROR_NOT_FOUND when the combat kit is not
 * in this world. Inside a query iteration the add is queued, so two calls for
 * one entity there in one step keep only the last amount. */
static inline EngResult combatDealDamage(EngWorld* world, EngEntity target, float amount) {
    const EngComponent inbox = engFindComponent(world, COMBAT_DAMAGE_INBOX);
    CombatDamageInbox in = { 0.0f };
    if (!inbox) return ENG_ERROR_NOT_FOUND;
    if (!(amount > 0.0f) || !engIsEntityAlive(world, target)) return ENG_OK;
    if (engHasComponent(world, target, inbox)) {
        EngResult r = engGetComponent(world, target, inbox, &in, (uint32_t)sizeof in);
        if (r != ENG_OK) return r;
        in.pending += amount;
        return engSetComponent(world, target, inbox, &in, (uint32_t)sizeof in);
    }
    in.pending = amount;
    return engAddComponent(world, target, inbox, &in);
}

ENG_EXTERN_C_END
#endif /* COMBAT_H */
