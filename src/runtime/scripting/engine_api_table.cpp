// ── Host side of the EngineApi table ────────────────────────────────────────
// One static table pointing every slot at the host's real engine* functions.
// Modules receive this via engineModuleBindApiV1 at load; see
// include/engine/engine_api_table.h for the contract and evolution rules.
#include <engine/engine_api_table.h>

// Host-internal (engine_api.cpp): whether a UI widget backend is registered.
bool engineUiHasBackend(void);

namespace {
// The table's ui.text takes PRE-FORMATTED text (function pointers can't be
// variadic); the client shim does the printf, this bridges to the host fn.
void uiTextPlain(const char* txt) { engineUiText("%s", txt); }
} // namespace

const EngineApiTableV1* engineApiHostTable(void) {
    // NOT static-const: the loader rebinds this per module load, and the ui
    // group is NEGOTIATED LIVE — its version is published only when a UI
    // backend actually exists (editor), else 0 = ABSENT (headless engine_host).
    // The client snapshots each group's version at bind time (engineApiHas),
    // so a group's presence tracks the host it loaded into, not a compile-time
    // assumption. Every other group is unconditional here; ui is the exemplar
    // for how a capability that a host may lack gets advertised.
    static EngineApiTableV1 t = {
        sizeof(EngineApiTableV1),
        { ENGINE_API_CORE_V,
          engineLogInfo, engineLogWarn, engineLogError,
          engineDeltaTime, engineElapsed, engineFrame,
          engineEntityCreate, engineEntityDestroy, engineEntityFind,
          engineEntityAlive, engineEntitySetParent, engineEntityClearParent,
          engineGetTransform, engineSetTransform },
        { ENGINE_API_INPUT_V,
          engineKeyDown, engineKeyPressed, engineAxis,
          engineMouseDown, engineMouseDelta,
          engineActionDown, engineActionPressed, engineActionReleased,
          engineActionAxis1, engineActionAxis2, engineLookDelta,
          engineSetCursorCaptured, engineCursorCaptured,
          engineLookTotal },
        { ENGINE_API_PHYSICS_V,
          engineApplyImpulse, engineSetVelocity, engineGetVelocity,
          engineRaycast, engineCharMove, engineCharJump, engineCharGrounded },
        { ENGINE_API_AUDIO_V,
          enginePlaySound, enginePlaySoundAt, engineStopSound },
        { ENGINE_API_ASSETS_V,
          engineAssetLoadMesh, engineAssetUnloadMesh,
          engineAssetLoadTexture, engineAssetUnloadTexture,
          engineAssetLoadMeshAsync, engineAssetLoadTextureAsync,
          engineAssetQueryMesh, engineAssetQueryTexture, engineAssetIsLoading,
          engineSceneLoad, engineSceneUnload,
          engineScenePreload, engineSceneIsReady },
        { ENGINE_API_ANIM_V,
          engineAnimPlay, engineAnimSetSpeed, engineAnimSetLooping,
          engineAnimSetPlaying, engineAnimIsPlaying,
          engineAnimTime, engineAnimDuration },
        { ENGINE_API_UI_V,
          uiTextPlain, engineUiButton, engineUiSliderFloat,
          engineUiCheckbox, engineUiSeparator,
          engineDrawLine, engineDrawSphere, engineDrawBox, engineDrawDisk },
        { ENGINE_API_NAV_V,
          engineNavFindPath, engineNavProject, engineNavReady },
    };
    // Re-published on every call: absent (0) until a UI backend registers.
    t.ui.version = engineUiHasBackend() ? ENGINE_API_UI_V : 0;
    return &t;
}
