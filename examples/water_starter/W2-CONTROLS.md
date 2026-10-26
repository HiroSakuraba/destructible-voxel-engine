# W2: Watercraft test harbor

Create a project using `dve_create_water PATH` or **Create water/ships test range** in the Gameplay Prototype Kit, then run `dve_player PATH`.

Craft selectors: **1** Dinghy, **2** Pirate Ship, **3** Sailboat, **4** Speedboat, **5** Wave Runner.

- W/S: motor throttle forward/reverse (dinghy, speedboat, wave runner), or increase/decrease sail exposure (sailboat/pirate).
- A/D: steer the rudder/helm.
- Q/E: sheet sails left/right, influencing angle of attack and mast force.
- P pause, R restart, F3 force and immersion diagnostics.

Start with the sailboat and hold W while adjusting Q/E to change the heading of the resulting sail force. Steer using A/D once moving. Change wind direction on the ocean marker to try upwind, beam reach and downwind conditions. Pirate ship responds much more slowly and develops heel because the center of effort is well above the hull.

Speedboat and wave runner use dynamic planing rather than simply floating at the wave height. Test acceleration and steering in calm and storm seas; jump over wakes once W3 wake forcing is integrated. W2 remains a physics-first, stylized water prototype; it does not simulate detailed sails or wake VFX.
