// ── abi_gate_module — a module built to be REFUSED ───────────────────────────
//
// `ModuleLibrary::load` runs a five-condition gauntlet before it lets a shared
// library touch the world: struct size, API version, ABI fingerprint, component
// layout hash, and kit-to-kit contracts. Every one of those exists to turn a
// silent memory-corruption bug into a refusal with a message.
//
// Until this fixture, NONE of them was exercised. `api_abi_compat_test` pins the
// function-table LAYOUTS — 13 groups, every offset — and that half is solid. But
// a layout regression is loud: an offset assert fires at compile time. A GATE
// regression is quiet. Reorder the checks, take an early `return false` that
// skips `destroy(t)`, invert a comparison, and the engine either loads a module
// it should have refused or leaks the module's table on every rejection. Nothing
// fails, nothing prints, and the symptom arrives days later as corrupted ECS
// memory in a hot-reload.
//
// The one test that dlopens a real module — `kit_lifecycle_test` — cannot cover
// this, and not by oversight: it needs `.so` files from the gitignored Kits
// repos, so `ctest -N` does not list it on a clean checkout and CI has never run
// it once. A gate that has never executed under test is a comment.
//
// ── Why one file and not six ────────────────────────────────────────────────
// Each defect is a SINGLE field set wrong, selected by -DABI_GATE_DEFECT at
// compile time, and CMake builds the same source into one module per value. Six
// hand-written fixtures would drift: someone updates the good one when the table
// grows and leaves five stale copies that now fail for the wrong reason and
// still look green. Here the control (defect 0) and every defect share one
// table-construction path, so a fixture can only differ in the field under test.
//
// ── What this deliberately does NOT link ────────────────────────────────────
// Nothing. Not engine_runtime — linking that into a module duplicates engine
// state, and the SDK says so. Not flecs either: `ecs_world_t*` appears only as
// an opaque pointer in the table's signatures and no flecs function is ever
// called, so no flecs symbol is referenced and the image has no undefined
// symbols at all. That is what lets these build as plain MODULE libraries on
// every platform, Windows included, where the hot_reload_game sample cannot.
#include <engine/game_module.h>
#include <engine/contract.h>

#include <cstdio>
#include <cstdlib>

#ifndef ABI_GATE_DEFECT
#  define ABI_GATE_DEFECT 0
#endif

// Defect codes. Kept as macros rather than an enum so the #if arms below read
// as the field they corrupt.
#define ABI_GATE_GOOD          0
#define ABI_GATE_STRUCT_SIZE   1
#define ABI_GATE_API_VERSION   2
#define ABI_GATE_FINGERPRINT   3
#define ABI_GATE_LAYOUT_HASH   4
#define ABI_GATE_NO_EXPORTS    5
#define ABI_GATE_NULL_TABLE    6
#define ABI_GATE_CONTRACT_V1   7
#define ABI_GATE_CONTRACT_V2   8

// ── The trace ───────────────────────────────────────────────────────────────
// Appends a line per lifecycle event to $ABI_GATE_TRACE. This is how the test
// observes something the exit status cannot show: that a REFUSED module still
// gets `destroy` called on its table. `refuse()` in module_loader.h is a lambda
// invoked from four separate arms of the gauntlet; if any one of them grows a
// bare `return false`, the module's table and instance leak on every rejected
// load. The trace turns that into a failing assertion instead of a slow drip
// only a sanitizer on a path nobody runs would ever notice.
//
// A file rather than a counter symbol: after a refusal the host parks the image
// in the graveyard and the test process is a separate program anyway, so there
// is nothing left to dlsym from the outside. Append mode, so a two-module
// sequence (the contract case) leaves both modules' events in load order.
static void trace(const char* event) {
    const char* path = std::getenv("ABI_GATE_TRACE");
    if (!path || !*path) return;
    if (std::FILE* f = std::fopen(path, "ab")) {
        std::fprintf(f, "%d %s\n", ABI_GATE_DEFECT, event);
        std::fclose(f);
    }
}

// Every hook is a no-op that records nothing. The gauntlet runs BEFORE any of
// these can be called, so a fixture that reached them would already have proved
// the point by being loaded at all — and the control fixture must be safe to
// actually attach.
static void gate_attach     (void*, void*)                 {}
static void gate_detach     (void*)                        {}
static void gate_simStart   (void*, ecs_world_t*)          {}
static void gate_simStop    (void*)                        {}
static void gate_update     (void*, ecs_world_t*, float)   {}
static void gate_physicsStep(void*, ecs_world_t*, float)   {}
static void gate_postPhysics(void*, ecs_world_t*)          {}

// A stand-in for the module's plugin instance. The host never dereferences
// `user` — only the module's own thunks do — but it must be a real allocation so
// the destroy path has something to free and a leak checker has something to
// find if destroy is skipped.
struct GateInstance { int marker = 0xA71; };

// ── Contract declarations ───────────────────────────────────────────────────
// Two fixtures declare the SAME contract name at DIFFERENT versions. Loaded in
// sequence into one host, the first pins "abi_gate" and the second must be
// refused. This is the only gate condition that cannot be provoked by a single
// module, because the registry it checks against starts empty.
#if ABI_GATE_DEFECT == ABI_GATE_CONTRACT_V1
ENGINE_MODULE_CONTRACTS({ "abi_gate", 1, 0x1111ull })
#elif ABI_GATE_DEFECT == ABI_GATE_CONTRACT_V2
ENGINE_MODULE_CONTRACTS({ "abi_gate", 2, 0x2222ull })
#endif

#if ABI_GATE_DEFECT != ABI_GATE_NO_EXPORTS

extern "C" ENGINE_MODULE_EXPORT
EngineGameModuleV1* engineGameModuleCreateV1(void) {
    trace("create");

#if ABI_GATE_DEFECT == ABI_GATE_NULL_TABLE
    // The module decides at runtime that it cannot initialise. The host has a
    // valid create symbol, calls it, and gets nothing back — a case with no
    // struct field to check, only a null test that a refactor can drop.
    return nullptr;
#else
    auto* inst = new GateInstance();
    auto* t    = new EngineGameModuleV1{};

    // The CORRECT values first, always. Each defect arm below overwrites exactly
    // one of them, so no fixture can accidentally differ in two fields and pass
    // the gate for a reason the test did not intend.
    t->structSize          = sizeof(EngineGameModuleV1);
    t->apiVersion          = ENGINE_GAME_API_VERSION;
    t->abiFingerprint      = ENGINE_ABI_FINGERPRINT;
    t->componentLayoutHash = engine_abi::componentLayoutHash();
    t->name                = "abi_gate_probe";
    t->version             = "1.0";
    t->user                = inst;
    t->attach              = gate_attach;
    t->detach              = gate_detach;
    t->simStart            = gate_simStart;
    t->simStop             = gate_simStop;
    t->update              = gate_update;
    t->physicsStep         = gate_physicsStep;
    t->postPhysics         = gate_postPhysics;
    t->loadReason          = nullptr;
    t->editorUi            = nullptr;
    t->frame               = nullptr;

#if ABI_GATE_DEFECT == ABI_GATE_STRUCT_SIZE
    // What a module built against an older or newer SDK reports. Understating
    // it is the dangerous direction — the host would read fields past the end
    // of the module's allocation — so that is the one tested.
    t->structSize = sizeof(EngineGameModuleV1) - sizeof(void*);
#elif ABI_GATE_DEFECT == ABI_GATE_API_VERSION
    // A stale rebuild: same compiler, same components, table reshaped.
    t->apiVersion = ENGINE_GAME_API_VERSION + 1;
#elif ABI_GATE_DEFECT == ABI_GATE_FINGERPRINT
    // A debug module against a release host. The string is deliberately
    // well-formed and plausible rather than garbage: the check is a string
    // COMPARE, and a fixture that fed it null would pass through the `!t->
    // abiFingerprint` short-circuit and never reach the comparison at all.
    t->abiFingerprint = "some-other-compiler|c++202002L|api4|debug";
#elif ABI_GATE_DEFECT == ABI_GATE_LAYOUT_HASH
    // A shared component grew a field since the host was built. World data
    // survives reloads, so this module would misread live ECS memory — the
    // one condition whose remedy is "restart the host", not "rebuild".
    t->componentLayoutHash = engine_abi::componentLayoutHash() ^ 0xDEADBEEFull;
#endif

    return t;
#endif  // NULL_TABLE
}

extern "C" ENGINE_MODULE_EXPORT
void engineGameModuleDestroyV1(EngineGameModuleV1* t) {
    // Traced BEFORE the null guard. A host that hands back null is still a host
    // that called destroy, and the test needs to tell "destroy ran" apart from
    // "destroy was skipped" without depending on what it was passed.
    trace("destroy");
    if (!t) return;
    delete static_cast<GateInstance*>(t->user);
    delete t;
}

#else   // ABI_GATE_NO_EXPORTS

// A shared library that loads perfectly and is not a game module: a mis-pathed
// project.json, a system library, a kit whose export macro was dropped. dlopen
// succeeds, both symbol lookups return null, and the host must say which
// symbols it wanted rather than crashing on a null call. One exported symbol so
// the image is not empty and the linker keeps it.
extern "C" ENGINE_MODULE_EXPORT int abiGateNotAModule(void) { return 1; }

#endif
