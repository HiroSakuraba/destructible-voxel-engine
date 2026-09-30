-- dve_player sample: spins the visual-only "Spinner" and lets move_x / lift move it.
-- `require` resolves through the game content (scripts/spinner.lua inside the .dvepak).
-- B blasts the tower (its top breaks off as a falling fragment); 1 / 2 save / load the
-- "slot1" save game from Lua; F5 / F9 quicksave / quickload (handled by the player).
-- Each blast schedules a named "aftershock" timer half a second later; named timers carry
-- plain data, so a save made in between still fires it after loading.
local spinner = require("spinner")

local id = world.find_by_name("Spinner")
if id == nil then
  world.log("Spinner not found")
  return
end

local state = spinner.new(id, world.get_position(id))
local tower = world.find_by_name("Tower")
local blasts = 0
local held = {}

-- True on the tick an action goes from released to pressed.
local function pressed(action)
  local now = world.is_action_pressed(action)
  local edge = now and not held[action]
  held[action] = now
  return edge
end

-- Named timer handler: registered at load time, so a restored timer finds it by name.
world.timer_handler("aftershock", function(data)
  world.set_global("aftershocks", data.blast)
  world.set_environment_scalar("Exposure", 1.0 + 0.25 * data.blast)
  world.log("aftershock " .. data.blast)
end)

local function blast_tower()
  if tower == nil then return end
  -- Cut a slab through the tower at 1.6 m so everything above it breaks off and falls.
  for _, x in ipairs({-1.6, -1.3, -1.0}) do
    for _, z in ipairs({-1.0, -0.7, -0.4}) do
      world.damage_sphere(tower, x, 1.6, z, 0.32)
    end
  end
  blasts = blasts + 1
  world.log("blast " .. blasts)
  world.schedule_once_named(0.5, "aftershock", { blast = blasts })
end

world.on_tick(function(dt)
  spinner.update(state, dt, world.get_axis("move_x"), world.is_action_pressed("lift"))
  if pressed("blast") then blast_tower() end
  if pressed("save_slot1") then
    local ok, err = world.save_game("slot1")
    if not ok then world.log("save failed: " .. tostring(err)) end
  end
  if pressed("load_slot1") then
    if world.save_exists("slot1") then world.load_game("slot1") else world.log("no slot1 save yet") end
  end
end)

-- Save games: the spinner lives in Lua locals, so hand it to the save and take it back.
world.on_save(function()
  return { spinner = spinner.save(state), blasts = blasts }
end)
world.on_load(function(saved)
  spinner.load(state, saved.spinner)
  blasts = saved.blasts or 0
end)
world.log("main.lua: spinner ready")
