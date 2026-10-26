# First-person prototype starter

Generate the complete example project, including its editable scene and runtime
assets:

```sh
cmake --build build --target dve_create_prototype dve_player
./build/dve_create_prototype /path/to/NEW_GAME
./build/dve_player /path/to/NEW_GAME
```

The generator refuses to overwrite an existing directory. The level is generated
from `src/editor_prototype.cpp`, keeping its editable and runtime geometry in sync.

In the editor, use **File > New Scene**, then **Tools > Gameplay Prototype Kit**
and **Create first-person starter**. Close the kit panel and press Play. The
panel also exports the current authored prototype to a fresh playable folder
inside an open project.

WASD moves; mouse looks; Space jumps; Ctrl crouches; E uses the switch; P pauses;
R restarts the prototype round; F3 shows the event trace. The exported game also
binds IJKL look and gamepad sticks/buttons. Escape exits the standalone player;
in the editor it ejects the prototype camera so editor controls are accessible.

Collect the three yellow items, use the green switch to open the gate, reach the
checkpoint, cross the red damage strip and enter the blue goal. Falling below the
kill plane or losing all health respawns at the checkpoint by default. Disable
`auto_respawn` on the Game Rules component to exercise the loss screen.

The kit is **Z up**, matching `GameplayRuntime`'s capsule controller. It configures
Z-down gravity in its own world. Ordinary scenes without prototype actors keep
their existing behavior. Lua is optional.

Package with:

```sh
./build/dve_pack /path/to/NEW_GAME /path/to/game.dvepak --all
./build/dve_player /path/to/game.dvepak
```

See [the implementation contract](../../docs/playable-prototype-kit-2026-10-24.md)
for event links, persistence, limits and current scope.
