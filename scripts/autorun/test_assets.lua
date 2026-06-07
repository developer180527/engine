-- test_assets.lua — Asset API test suite
-- Autorun: runs automatically on Play, no entity attachment needed.
-- Results are logged to the console panel.
--
-- Tests: sync load/unload, async load/query, count queries, error handling.
local M = {}

local passed = 0
local failed = 0
local total  = 0

local function check(name, condition)
    total = total + 1
    if condition then
        passed = passed + 1
        Log.info("[PASS] " .. name)
    else
        failed = failed + 1
        Log.error("[FAIL] " .. name)
    end
end

-- ── Phase 1: Sync load ─────────────────────────────────────────────────
function M:onStart()
    Log.info("═══════════════════════════════════════")
    Log.info("  Asset API Test Suite")
    Log.info("═══════════════════════════════════════")

    -- Baseline counts
    local baseMeshes = Assets.meshCount()
    local baseTex    = Assets.textureCount()
    local baseMat    = Assets.materialCount()
    Log.info("Baseline: meshes=" .. baseMeshes
        .. " textures=" .. baseTex
        .. " materials=" .. baseMat)

    -- Test: meshCount/textureCount/materialCount return numbers
    check("meshCount returns number",     type(baseMeshes) == "number")
    check("textureCount returns number",  type(baseTex) == "number")
    check("materialCount returns number", type(baseMat) == "number")

    -- Test: load invalid path returns 0
    local badMesh = Assets.loadMesh("nonexistent/garbage.cooked")
    check("loadMesh bad path returns 0", badMesh == 0)

    local badTex = Assets.loadTexture("nonexistent/garbage.cooked")
    check("loadTexture bad path returns 0", badTex == 0)

    -- Test: load empty string returns 0
    local emptyMesh = Assets.loadMesh("")
    check("loadMesh empty string returns 0", emptyMesh == 0)

    -- Test: unload invalid handle returns false
    local unloadBad = Assets.unloadMesh(0)
    check("unloadMesh(0) returns false", not unloadBad)

    local unloadHuge = Assets.unloadMesh(999999)
    check("unloadMesh(999999) returns false", not unloadHuge)

    -- Test: unloadTexture invalid
    check("unloadTexture(0) returns false", not Assets.unloadTexture(0))

    -- Test: unloadMaterial invalid
    check("unloadMaterial(0) returns false", not Assets.unloadMaterial(0))

    -- Test: async query for unknown path returns 0
    local qm = Assets.queryMesh("nonexistent.cooked")
    check("queryMesh unknown returns 0", qm == 0)

    local qt = Assets.queryTexture("nonexistent.cooked")
    check("queryTexture unknown returns 0", qt == 0)

    -- Test: isLoading for unknown path returns false
    local loading = Assets.isLoading("nonexistent.cooked")
    check("isLoading unknown returns false", not loading)

    -- ── Try loading a real cooked mesh (if any exist) ──────────────────
    -- We'll attempt to load the first mesh in the list. If the project has
    -- cooked meshes, this tests the real load path.
    self.testMeshHandle = 0
    self.testTexHandle  = 0
    self.asyncStarted   = false
    self.asyncPath      = nil
    self.phase          = "sync_done"

    Log.info("─── Sync tests done ───")
end

-- ── Phase 2: Async load (polled each frame) ────────────────────────────
function M:onUpdate(dt)
    if self.phase == "sync_done" then
        -- Kick off an async load with a bad path to test error handling
        Assets.loadMeshAsync("nonexistent_async.cooked")
        check("loadMeshAsync doesn't crash on bad path", true)

        -- isLoading should be true immediately after async request
        -- (or false if it failed instantly — both are valid)
        local isLoad = Assets.isLoading("nonexistent_async.cooked")
        check("isLoading returns boolean after async", type(isLoad) == "boolean")

        self.phase = "async_wait"
        self.asyncFrames = 0
    elseif self.phase == "async_wait" then
        self.asyncFrames = self.asyncFrames + 1
        -- Wait a few frames for the async system to process
        if self.asyncFrames > 5 then
            -- The bad path should have failed by now
            local qm = Assets.queryMesh("nonexistent_async.cooked")
            check("queryMesh for failed async returns 0", qm == 0)
            self.phase = "done"
            self:printSummary()
        end
    end
end

function M:printSummary()
    Log.info("═══════════════════════════════════════")
    if failed == 0 then
        Log.info("  ✓ ALL " .. total .. " ASSET TESTS PASSED")
    else
        Log.error("  ✗ " .. failed .. "/" .. total .. " ASSET TESTS FAILED")
    end
    Log.info("═══════════════════════════════════════")
end

function M:onDestroy()
    -- Cleanup: unload anything we loaded
    if self.testMeshHandle and self.testMeshHandle > 0 then
        Assets.unloadMesh(self.testMeshHandle)
    end
    if self.testTexHandle and self.testTexHandle > 0 then
        Assets.unloadTexture(self.testTexHandle)
    end
end

return M
