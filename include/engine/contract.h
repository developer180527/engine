#pragma once
/* ── Kit component contracts — versioned, layout-checked at load ─────────────
 *
 * Kits cooperate through shared ECS components declared in contract headers
 * (e.g. CombatKit's combat_contract.h). flecs matches those types ACROSS
 * modules by name — which is exactly why version skew is lethal: kit A built
 * against v1's struct layout and kit B against v2's write the same component
 * id with different bytes. The compiler can't see across dylibs; this
 * gauntlet can.
 *
 * A contract header declares its identity:
 *
 *     namespace combat {
 *     constexpr uint32_t kContractVersion = 2;
 *     constexpr uint64_t kContractLayout  =            // sizeof fold — any
 *         sizeof(Health) | (sizeof(DamageInbox) << 8)  // layout change moves it
 *                        | (sizeof(Died) << 16);
 *     }
 *     #define COMBAT_CONTRACT \
 *         { "combat", combat::kContractVersion, combat::kContractLayout }
 *
 * Every module that INCLUDES a contract (publisher or consumer alike) exports
 * its view of it, once, next to ENGINE_GAME_MODULE:
 *
 *     ENGINE_MODULE_CONTRACTS(COMBAT_CONTRACT)
 *
 * The loader collects declarations across all loaded modules; the first
 * module to declare a contract pins it, and any later module whose version or
 * layout differs is REFUSED at load with a message naming both sides —
 * a build error's moral equivalent, delivered at the last safe moment.
 */
#include <stdint.h>
#include "engine_export.h"

extern "C" {
typedef struct EngineContractDecl {
    const char* name;      /* contract identity, e.g. "combat"            */
    uint32_t    version;   /* bump on any semantic/layout change          */
    uint64_t    layout;    /* compile-time fold of the component sizes    */
} EngineContractDecl;

typedef const EngineContractDecl* (*EngineModuleContractsV1Fn)(int* count);
}

#define ENGINE_MODULE_CONTRACTS(...)                                         \
    extern "C" ENGINE_MODULE_EXPORT                        \
    const EngineContractDecl* engineModuleContractsV1(int* count) {          \
        static const EngineContractDecl k[] = { __VA_ARGS__ };               \
        *count = (int)(sizeof(k) / sizeof(k[0]));                            \
        return k;                                                            \
    }
