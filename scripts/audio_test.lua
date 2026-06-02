-- Audio smoke test. Attach to any entity, press Play — plays once.
-- Path is RELATIVE to <project>/assets (resolve() prepends assetsRoot),
-- so do NOT include a leading "assets/".
local M = {}
function M:onStart()
    self.played = false
end
function M:onUpdate(dt)
    if not self.played then
        self.played = true
        Audio.play("Audio/background_music.wav")
    end
end
return M
