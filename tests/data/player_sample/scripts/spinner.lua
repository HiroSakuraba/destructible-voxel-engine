local M = {}

function M.new(id, position)
  return { id = id, x = position.x, y = position.y, z = position.z, base_y = position.y, angle = 0 }
end

function M.update(state, dt, move_x, lift)
  state.angle = (state.angle + 90 * dt) % 360
  state.x = state.x + move_x * 1.5 * dt
  local target_y = lift and (state.base_y + 0.6) or state.base_y
  state.y = state.y + (target_y - state.y) * math.min(1, 6 * dt)
  world.set_rotation(state.id, 0, state.angle, 0)
  world.set_position(state.id, state.x, state.y, state.z)
  world.set_global("spinner_x", state.x)
end

-- Plain data for world.on_save (the object id is stable across a save and load).
function M.save(state)
  return { x = state.x, y = state.y, z = state.z, base_y = state.base_y, angle = state.angle }
end

function M.load(state, saved)
  if saved == nil then return end
  for key, value in pairs(saved) do state[key] = value end
end

return M
