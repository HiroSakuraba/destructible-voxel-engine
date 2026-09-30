-- dve_player sample: spins the visual-only "Spinner" and lets move_x / lift move it.
-- `require` resolves through the game content (scripts/spinner.lua inside the .dvepak).
local spinner = require("spinner")

local id = world.find_by_name("Spinner")
if id == nil then
  world.log("Spinner not found")
  return
end

local state = spinner.new(id, world.get_position(id))
world.on_tick(function(dt)
  spinner.update(state, dt, world.get_axis("move_x"), world.is_action_pressed("lift"))
end)
world.log("main.lua: spinner ready")
