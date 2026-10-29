-- GI showcase: Q/E turn the sun around the room, Z/X lower/raise it, R resets,
-- T toggles an ambient-only view (no sun, white sky) where the image IS the occlusion.
local azimuth, elevation = 0.55, 0.95   -- radians
local function apply()
  local c = math.cos(elevation)
  world.set_environment_vector("SunDirection", math.cos(azimuth) * c, math.sin(elevation), math.sin(azimuth) * c)
end
world.set_environment_scalar("SunIntensity", 3.0)
world.set_environment_scalar("AmbientOcclusionSamples", 16)
world.set_environment_scalar("AmbientOcclusionMaxDistanceMeters", 0.9)
local aoView, held = false, false
local function look()
  if aoView then
    world.set_environment_scalar("SunIntensity", 0.0)
    world.set_environment_vector("SkyColor", 1.0, 1.0, 1.0)
    world.set_environment_vector("GroundColor", 1.0, 1.0, 1.0)
  else
    world.set_environment_scalar("SunIntensity", 3.0)
    world.set_environment_vector("SkyColor", 0.50, 0.62, 0.90)
    world.set_environment_vector("GroundColor", 0.30, 0.28, 0.25)
  end
end
look()
apply()
world.on_tick(function(dt)
  local turn = 1.2 * dt
  if world.is_action_pressed("sun_left") then azimuth = azimuth - turn end
  if world.is_action_pressed("sun_right") then azimuth = azimuth + turn end
  if world.is_action_pressed("sun_up") then elevation = math.min(1.5, elevation + 0.6 * dt) end
  if world.is_action_pressed("sun_down") then elevation = math.max(0.15, elevation - 0.6 * dt) end
  local t = world.is_action_pressed("ao_view")
  if t and not held then aoView = not aoView; look() end
  held = t
  if world.is_action_pressed("sun_reset") then azimuth, elevation = 0.55, 0.95 end
  apply()
end)
