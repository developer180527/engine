-- spin.lua — rotates the owning entity around the Y axis.
-- Demonstrates: instance state (self.t), entity handle (self.entity),
-- Transform get/set, Time, Log.
local M = {}

function M:onStart()
    self.t = 0
    Log.info("spin.lua started on '" .. self.entity:name() .. "'")
end

function M:onUpdate(dt)
    self.t = self.t + dt
    local tr = self.entity:getTransform()
    local a  = self.t                 -- angle in radians (accumulates)
    tr.rotation.x = 0
    tr.rotation.y = math.sin(a * 0.5) -- quaternion for Y-axis rotation
    tr.rotation.z = 0
    tr.rotation.w = math.cos(a * 0.5)
    self.entity:setTransform(tr)
end

function M:onDestroy()
    Log.info("spin.lua stopped")
end

return M
