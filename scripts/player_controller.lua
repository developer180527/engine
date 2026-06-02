-- First-person controller: mouse-look (drives a child "PlayerCamera"),
-- WASD relative to facing, Space jump. Attach to a CharacterController entity.
local M = {}
local function clamp(v, lo, hi) if v<lo then return lo elseif v>hi then return hi else return v end end

function M:onStart()
    self.speed=5.0; self.jumpSpeed=6.0; self.sens=0.0025; self.eyeHeight=1.6
    self.yaw=0.0; self.pitch=0.0
    self.camera = World.find("PlayerCamera")
end

function M:onUpdate(dt)
    local dx, dy = Input.mouseDelta()
    dx = clamp(dx,-100,100); dy = clamp(dy,-100,100)
    self.yaw   = self.yaw   - dx * self.sens
    self.pitch = clamp(self.pitch + dy * self.sens, -1.4, 1.4)

    -- body yaw (camera child inherits it)
    local hy = self.yaw*0.5
    local t = self.entity:getTransform()
    t.rotation = { x=0, y=math.sin(hy), z=0, w=math.cos(hy) }
    self.entity:setTransform(t)

    -- child camera: local head offset + pitch
    if self.camera and self.camera:isAlive() then
        local hp = self.pitch*0.5
        local ct = self.camera:getTransform()
        ct.position = { x=0, y=self.eyeHeight, z=0 }
        ct.rotation = { x=math.sin(hp), y=0, z=0, w=math.cos(hp) }
        self.camera:setTransform(ct)
    end

    -- move relative to facing
    local fwd,strafe = 0,0
    if Input.keyDown("W") then fwd=fwd+1 end
    if Input.keyDown("S") then fwd=fwd-1 end
    if Input.keyDown("A") then strafe=strafe+1 end
    if Input.keyDown("D") then strafe=strafe-1 end
    if fwd~=0 and strafe~=0 then fwd=fwd*0.7071; strafe=strafe*0.7071 end
    local fx,fz = -math.sin(self.yaw), -math.cos(self.yaw)
    local rx,rz =  math.cos(self.yaw), -math.sin(self.yaw)
    self.entity:move((fx*fwd+rx*strafe)*self.speed, (fz*fwd+rz*strafe)*self.speed)

    if Input.keyPressed("Space") and self.entity:isGrounded() then self.entity:jump(self.jumpSpeed) end
end
return M
