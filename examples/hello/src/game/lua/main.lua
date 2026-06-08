local slot = -1
local frame = 0

function init()
    blyt.buf.alloc_slot(S.GLOBALS)
    slot = blyt.buf.alloc_slot(S.PLAYER)
    S.player[slot].x = 0
    S.player[slot].y = 0
    blyt.debug.print("init player pos: 0, 0")
end

function update()
    frame = frame + 1
    if frame % 10 == 0 then
        local x = (S.player[slot].x + 1) % 320
        local y = (S.player[slot].y + 1) % 240
        S.player[slot].x = x
        S.player[slot].y = y
        blyt.debug.print("update frame " .. frame .. " player pos: " .. x .. ", " .. y)
    end
end

function draw()
    if frame % 10 == 0 then
        blyt.debug.print("draw frame " .. frame ..
                         " player pos: " .. S.player[slot].x ..
                         ", " .. S.player[slot].y)
    end
end

function on_save_state()
    S.globals[0].frame = frame
end

function on_load_state(info)
    frame = S.globals[0].frame
    slot = 0
end
