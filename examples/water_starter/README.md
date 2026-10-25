# W1 — Water and Ship Test Range

Build `dve_create_water` and run `dve_create_water NEW_PROJECT_DIRECTORY`, or create the Water/Ships test range in the editor's Gameplay Prototype Kit panel. Launch the exported project with `dve_player`.

Switch between two vessels sharing one Gerstner ocean: a nimble 3×3 probe dinghy and a 9×5 probe pirate ship. W/S controls simple motor thrust; A/D controls rudder; 1/2 selects the boat; E toggles; P pauses; R restarts; F3 displays buoyancy and roll/pitch telemetry.

Change the `dve.prototype.water_ocean` marker's `sea_state` property to 0 (Calm), 1 (Breeze), or 2 (Storm), then export/restart. Water physics and the player-rendered ocean both sample the same analytic wave field. Water itself does **not** collide with ships as a static box; submerged hull probes generate buoyancy and damping.

Future W2 adds sails, keel response, planing and jet propulsion; W3 wakes/spray; W4 surfing and nearshore breaking waves. The W1 visual harbor uses procedural geometry only.
