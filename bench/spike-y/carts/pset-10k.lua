-- #208 / Spike Y per-pixel workload: plot a W×H region each frame (pset form).
local set_pixel = blyt32.surface.set_pixel
local pset = blyt32.surface.pset
local W, H = 100, 100
local t = 0
function init() end
function update() t = t + 1 end
function draw()
  local lk = blyt32.surface.acquire(blyt32.surface.SCREEN)
  for y=0,H-1 do for x=0,W-1 do pset(lk,x,y,(x+y+t)&0xFF) end end
  lk:release()
end
