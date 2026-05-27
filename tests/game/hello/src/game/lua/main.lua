local frame = 0

function init()
    blyt32.debug.print("hello from lua")
end

function update()
    frame = frame + 1
    if frame % 60 == 0 then
        blyt32.debug.print("update")
    end
end

function draw()
    if frame % 60 == 0 then
        blyt32.debug.print("draw")
    end
end
