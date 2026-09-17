-- DVE v1.45 playable-runtime example.
-- `pawn` is expected to be a GameObjectId tagged "player".

local player = world.player_create("Local Player", true)
world.character_add(pawn, {
    maximum_ground_speed = 5.5,
    step_height = 0.35,
    maximum_slope_degrees = 50.0,
    jump_speed = 7.0
})
world.player_possess(player, pawn)

local checkpoint = world.trigger_create_box(
    "Checkpoint", 4.0, 0.0, 1.0, 1.0, 2.0, 1.5, "player", true)

world.on_trigger(function(trigger_id, kind, object_id, fixed_tick)
    if trigger_id == checkpoint and kind == "enter" then
        print("Checkpoint reached by", object_id, "at fixed tick", fixed_tick)
    end
end)

function fixed_update(move_x, move_y, jump_pressed, crouch_held)
    world.player_set_input(player, move_x, move_y, jump_pressed, crouch_held)
end
