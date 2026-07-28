local M = {}
function M:onUpdate(dt)
    if Input.keyDown("Space") then self.entity:setVelocity(0, 6, 0) end
end
function M:onCollisionEnter(other)
    Log.info("hit: " .. other:name())
end
return M