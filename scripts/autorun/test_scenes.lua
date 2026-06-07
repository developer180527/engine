-- test_scenes.lua — Scene API test suite
-- Autorun: runs automatically on Play, no entity attachment needed.
-- Results are logged to the console panel.
--
-- Tests: load/unload, preload/isReady, error handling, entity tracking.
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

function M:onStart()
    Log.info("═══════════════════════════════════════")
    Log.info("  Scene API Test Suite")
    Log.info("═══════════════════════════════════════")

    -- Test: activeCount starts at 0 (or whatever the current count is)
    local baseActive = Scene.activeCount()
    check("activeCount returns number", type(baseActive) == "number")
    self.baseActive = baseActive

    -- Test: load invalid path returns 0
    local badHandle = Scene.load("nonexistent/bad_scene.cooked")
    check("Scene.load bad path returns 0", badHandle == 0)

    -- Test: load empty string returns 0
    local emptyHandle = Scene.load("")
    check("Scene.load empty string returns 0", emptyHandle == 0)

    -- Test: unload invalid handle returns false
    local unloadBad = Scene.unload(0)
    check("Scene.unload(0) returns false", not unloadBad)

    local unloadHuge = Scene.unload(999999)
    check("Scene.unload(999999) returns false", not unloadHuge)

    -- Test: entityCount for invalid handle returns 0
    local ecBad = Scene.entityCount(0)
    check("Scene.entityCount(0) returns 0", ecBad == 0)

    local ecHuge = Scene.entityCount(999999)
    check("Scene.entityCount(999999) returns 0", ecHuge == 0)

    -- Test: activeCount unchanged after failed loads
    local afterFails = Scene.activeCount()
    check("activeCount unchanged after failed loads",
          afterFails == self.baseActive)

    -- Test: isReady for unknown path returns false
    local ready = Scene.isReady("nonexistent.cooked")
    check("isReady unknown returns false", not ready)

    -- Test: preload doesn't crash on bad path
    Scene.preload("nonexistent_preload.cooked")
    check("preload doesn't crash on bad path", true)

    -- ── Async preload test ─────────────────────────────────────────────
    self.phase = "preload_wait"
    self.waitFrames = 0

    Log.info("─── Sync tests done, waiting for async ───")
end

function M:onUpdate(dt)
    if self.phase == "preload_wait" then
        self.waitFrames = self.waitFrames + 1
        if self.waitFrames > 5 then
            -- After a few frames, the bad preload should have settled
            local ready = Scene.isReady("nonexistent_preload.cooked")
            -- isReady returns false for unknown/failed preloads
            check("isReady for failed preload is boolean", type(ready) == "boolean")

            self.phase = "done"
            self:printSummary()
        end
    end
end

function M:printSummary()
    Log.info("═══════════════════════════════════════")
    if failed == 0 then
        Log.info("  ✓ ALL " .. total .. " SCENE TESTS PASSED")
    else
        Log.error("  ✗ " .. failed .. "/" .. total .. " SCENE TESTS FAILED")
    end
    Log.info("═══════════════════════════════════════")
end

function M:onDestroy()
    -- Nothing to clean up — all loads were intentionally invalid
end

return M
