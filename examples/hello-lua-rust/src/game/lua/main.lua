local greeting = require("greeting")

local frame = 0

function init()
    greeting.hello()
end

function update()
    frame = frame + 1
    if frame % 60 == 0 then
        greeting.hello()
    end
end

function draw()
end
