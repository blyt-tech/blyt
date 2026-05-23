local frame = 0

function init()
    blyt32.debug.print("hello from lua")
end

function update()
    frame = frame + 1
    if frame >= 2 then
        blyt.quit()
    end
end

function draw()
end
