local greeting = require("greeting")

local frame = 0

function init()
    greeting.log("hello from lua+rust")
end

function update()
    frame = frame + 1
    if frame % 60 == 0 then
        greeting.log("update " .. frame)
    end
end

function draw()
    if frame % 60 == 0 then
        greeting.log("draw " .. frame)
    end
end
