-- hello-assets — the pure-Lua resource API (#93).
--
-- Identical player-movement behaviour to examples/hello (so it satisfies the
-- shared examples smoke-test), plus it loads the `greeting` text resource by
-- handle and prints its contents.  The R.GREETING constant comes from the
-- packer-generated `cart_resources` module (ADR-0040); the bytes come from
-- assets/greeting.txt.

local R = require("cart_resources")

-- frame is deliberately plain Lua state (not a state buffer) to demonstrate
-- serialising static state in on_save_state/on_load_state.
local frame

function init()
    frame = 0
    -- R.GREETING is a typed text-resource constant (#166); :text() returns an
    -- owned Lua string (pin -> copy -> unpin), safe to keep beyond the frame.
    -- The resource is referenced by its constant directly — no load/release
    -- handle to track (ADR-0134, #196); the runtime owns residency.
    blyt.debug.print(R.GREETING:text())
end

function on_new_state()
    blyt.buf.alloc_slot(S.GLOBALS)
    local slot = blyt.buf.alloc_slot(S.CHARACTER)
    S.globals[0].player = blyt.buf.ref(S.CHARACTER, slot)
    S.character[slot].x = 160
    S.character[slot].y = 120
    blyt.debug.print("init player pos: 160, 120")
end

function update()
    frame = frame + 1
    if frame % 10 == 0 then
        local player = S.globals[0].player
        if blyt.buf.ref_valid(S.CHARACTER, player) then
            local slot = blyt.buf.ref_slot(player)
            local x = (S.character[slot].x + 1) % 320
            local y = (S.character[slot].y + 1) % 240
            S.character[slot].x = x
            S.character[slot].y = y
            blyt.debug.print("update frame " .. frame .. " player pos: " .. x .. ", " .. y)
        end
    end
end

function draw()
    if frame % 10 == 0 then
        local slot = blyt.buf.ref_slot(S.globals[0].player)
        blyt.debug.print(
            "draw frame "
                .. frame
                .. " player pos: "
                .. S.character[slot].x
                .. ", "
                .. S.character[slot].y
        )
    end
end

function on_save_state()
    S.globals[0].frame = frame
end

function on_load_state(_info)
    frame = S.globals[0].frame
end
