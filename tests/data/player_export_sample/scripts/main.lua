-- dve_player_export_scene: the exported components and tags are visible to scripts.
local carts = world.find_by_component("game.cart")
local vehicles = world.find_by_tag("vehicle")
world.log("export sample: carts=" .. #carts .. " vehicles=" .. #vehicles)
