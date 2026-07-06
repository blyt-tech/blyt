-- doom_bench.lua — Spike B doom_tick workload as a callable bench(REPEATS).
-- Faithful port of spikes/spike-b/benchmarks/doom_tick.lua: Doom-shaped per-tic
-- logic (IDLE/CHASE/ATTACK/DEAD state machine, math.sqrt range query per tic,
-- projectile table.insert/remove churn, LCG-driven DEAD transitions). One
-- run_one_sim() = 100 frames x 64 mobs; each re-seeds the LCG so every sim is
-- identical and bench(n) returns n * single_sim_result (integer) — a checksum
-- that BOTH the native and emulated legs must print identically.
local sqrt = math.sqrt
local floor = math.floor
local STATE_IDLE, STATE_CHASE, STATE_ATTACK, STATE_DEAD = 1, 2, 3, 4
local PLAYER_X, PLAYER_Y = 64.0, 64.0
local SIGHT_RANGE, ATTACK_RANGE, ATTACK_PERIOD = 40.0, 12.0, 8
local CHASE_SPEED, PROJECTILE_SPEED, PROJECTILE_TTL = 1.5, 4.0, 30
local N_MOBS, NFRAMES = 64, 100

local function run_one_sim()
    local seed = 12345
    local function lrand()
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        return seed / 2147483647
    end
    local mobs = {}
    for i = 1, N_MOBS do
        mobs[i] = { x = lrand() * 128.0, y = lrand() * 128.0, vx = 0.0, vy = 0.0,
                    hp = 10, state = STATE_IDLE, tics = 0, alive = true }
    end
    local projectiles = {}
    local function spawn_projectile(from)
        local dx, dy = PLAYER_X - from.x, PLAYER_Y - from.y
        local d = sqrt(dx * dx + dy * dy)
        if d < 0.001 then d = 1.0 end
        projectiles[#projectiles + 1] = { x = from.x, y = from.y,
            vx = dx / d * PROJECTILE_SPEED, vy = dy / d * PROJECTILE_SPEED, ttl = PROJECTILE_TTL }
    end
    local function tick_mob(m)
        if not m.alive then return end
        m.tics = m.tics + 1
        local dx, dy = PLAYER_X - m.x, PLAYER_Y - m.y
        local dist = sqrt(dx * dx + dy * dy)
        local s = m.state
        if s == STATE_IDLE then
            if dist < SIGHT_RANGE then m.state = STATE_CHASE; m.tics = 0 end
        elseif s == STATE_CHASE then
            if dist < ATTACK_RANGE then m.state = STATE_ATTACK; m.tics = 0
            else
                if dist > 0.001 then m.vx = dx / dist * CHASE_SPEED; m.vy = dy / dist * CHASE_SPEED end
                m.x = m.x + m.vx; m.y = m.y + m.vy
            end
        elseif s == STATE_ATTACK then
            if dist > ATTACK_RANGE then m.state = STATE_CHASE
            elseif (m.tics % ATTACK_PERIOD) == 0 then spawn_projectile(m) end
        elseif s == STATE_DEAD then m.alive = false end
    end
    local function tick_projectiles()
        local n = #projectiles
        for i = 1, n do
            local p = projectiles[i]
            p.x = p.x + p.vx; p.y = p.y + p.vy; p.ttl = p.ttl - 1
        end
        for i = n, 1, -1 do
            local p = projectiles[i]
            if p.ttl <= 0 or p.x < 0 or p.x > 128 or p.y < 0 or p.y > 128 then
                table.remove(projectiles, i)
            end
        end
    end
    local function maybe_kill_one()
        if lrand() < 0.02 then
            local m = mobs[floor(lrand() * N_MOBS) + 1]
            if m.alive then m.state = STATE_DEAD end
        end
    end
    for _ = 1, NFRAMES do
        for i = 1, N_MOBS do tick_mob(mobs[i]) end
        tick_projectiles()
        maybe_kill_one()
    end
    local total = #projectiles
    for i = 1, N_MOBS do total = total + (mobs[i].alive and 1 or 0) end
    return total
end

function bench(n)
    local total = 0
    for _ = 1, n do total = total + run_one_sim() end
    return total
end
