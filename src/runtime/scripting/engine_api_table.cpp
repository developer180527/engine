// ── Host side of the EngineApi table ────────────────────────────────────────
// One static table pointing every slot at the host's real engine* functions.
// Modules receive this via engineModuleBindApiV1 at load; see
// include/engine/engine_api_table.h for the contract and evolution rules.
#include <engine/engine_api_table.h>

namespace {
// The table's ui.text takes PRE-FORMATTED text (function pointers can't be
// variadic); the client shim does the printf, this bridges to the host fn.
void uiTextPlain(const char* txt) { engineUiText("%s", txt); }
} // namespace

const EngineApiTableV1* engineApiHostTable(void) {
    static const EngineApiTableV1 t = {
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
    };
    return &t;
}
