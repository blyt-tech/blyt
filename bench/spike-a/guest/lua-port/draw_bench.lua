-- draw_bench.lua — Doom R_DrawColumn analog: affine paletted texture-mapped
-- vertical columns with a colormap (light) lookup, written to a Lua framebuffer.
-- This is the ALL-LUA software-renderer case — the per-pixel path that takes the
-- emulated penalty (contrast: a native gfx/blit primitive would be leg-neutral).
-- Pure fixed-point integer (like Doom's real column drawer), so bench(N) returns
-- an integer FNV-1a checksum of the 320x240 framebuffer that BOTH legs must match
-- (render-determinism cross-check). i32 multiply wraps mod 2^32 identically on
-- native-int32 and RV32-int32 (BLYT_LUA_I32_F64).
local W, H = 320, 240
local TW, TH = 64, 128 -- Doom-ish 64-wide texture
local FRACBITS = 16
local FRACUNIT = 1 << FRACBITS
local TEXMASK = TH - 1
local FRACWRAP = FRACUNIT * TH - 1

local tex = {}
for i = 0, TW * TH - 1 do
    tex[i] = (i * 1103515245 + 12345) & 0xff
end
local cm = {} -- colormap: texel -> shaded palette index (one light level)
for i = 0, 255 do
    cm[i] = (i * 7 + 13) & 0xff
end
local fb = {}
for i = 0, W * H - 1 do
    fb[i] = 0
end

-- One frame: every screen column drawn as a full-height affine textured column
-- (dest steps by W per pixel, exactly like Doom's dest += SCREENWIDTH).
local function draw_frame()
    for x = 0, W - 1 do
        local col = (x & (TW - 1)) * TH
        local step = (FRACUNIT * TH) // H + ((x & 15) << (FRACBITS - 6))
        local frac = (x * 131) & FRACWRAP
        local o = x
        for _ = 0, H - 1 do
            fb[o] = cm[tex[col + ((frac >> FRACBITS) & TEXMASK)]]
            o = o + W
            frac = frac + step
        end
    end
end

function bench(n)
    for _ = 1, n do
        draw_frame()
    end
    local h = 0x811c9dc5
    for i = 0, W * H - 1 do
        h = (h ~ fb[i]) * 0x01000193
    end
    return h & 0x7fffffff
end
