-- Press Space to jump, but only when grounded (raycast down finds a surface).
-- Attach to a dynamic-RigidBody entity sitting above a static floor.
local M = {}

function M:onUpdate(dt)
    if Input.keyPressed("Space") then
        local t = self.entity:getTransform()
        local hit = Physics.raycast(t.position, { x = 0, y = -1, z = 0 }, 1.2)
        if hit.hit then
            self.entity:applyImpulse(0, 6, 0)
            Log.info(string.format("jump — ground %.2fm below", hit.distance))
        else
            Log.info("airborne, no jump")
        end
    end
end

return M
