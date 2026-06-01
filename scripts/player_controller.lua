-- WASD move + Space jump for a CharacterController entity.
local M = {}

function M:onStart()
    self.speed     = 5.0
    self.jumpSpeed = 6.0
end

function M:onUpdate(dt)
    local x, z = 0, 0
    if Input.keyDown("W") then z = z - 1 end   -- forward = -Z
    if Input.keyDown("S") then z = z + 1 end
    if Input.keyDown("A") then x = x + 1 end    -- (flipped vs before)
    if Input.keyDown("D") then x = x - 1 end
    if x ~= 0 and z ~= 0 then x, z = x * 0.7071, z * 0.7071 end  -- normalize diagonal
    self.entity:move(x * self.speed, z * self.speed)

    if Input.keyPressed("Space") and self.entity:isGrounded() then
        self.entity:jump(self.jumpSpeed)
    end
end

return M
