-- Controls for inspecting dynamic indirect lighting in Sponza.
local azimuth, elevation = 0.55, 0.95
local giEnabled, held = true, false
local function applySun()
  local c = math.cos(elevation)
  world.set_environment_vector("SunDirection", math.cos(azimuth) * c, math.sin(elevation), math.sin(azimuth) * c)
end
world.set_environment_scalar("SunIntensity", 3.0)
world.set_environment_scalar("GlobalIlluminationIntensity", 0.65)
applySun()
world.on_tick(function(dt)
  if world.is_action_pressed("sun_left") then azimuth = azimuth - 1.2 * dt end
  if world.is_action_pressed("sun_right") then azimuth = azimuth + 1.2 * dt end
  if world.is_action_pressed("sun_up") then elevation = math.min(1.5, elevation + 0.6 * dt) end
  if world.is_action_pressed("sun_down") then elevation = math.max(0.15, elevation - 0.6 * dt) end
  if world.is_action_pressed("sun_reset") then azimuth, elevation = 0.55, 0.95 end
  local pressed = world.is_action_pressed("gi_toggle")
  if pressed and not held then
    giEnabled = not giEnabled
    world.set_environment_scalar("GlobalIlluminationIntensity", giEnabled and 0.65 or 0.0)
  end
  held = pressed
  applySun()
end)
