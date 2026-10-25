# W4 Surf Beach

Generate the Water Ships starter project and run it in the native player.

Press **6** to control the new surfboard offshore of the added beach. Press **W** to paddle into shore, then **A/D** to lean and carve while catching the moving wave face. The board uses the surface slope and water velocity from the shared ocean, not a scripted forward shove. Look for foamy breaking sections as the wave approaches shallow water; wipeouts return the board to its starting position.

Choose **1–5** for the original dinghy, pirate ship, sailboat, speedboat and wave runner. These vessels still use the W1/W2 buoyancy and propulsion systems and the W3 physically interactive wakes. Tab cycles all six craft, P pauses, and F3 shows water forces and surfing diagnostics.

For authoring, select the **Surf Beach Waves** marker. Its world Y position sets the waterline; `beach_slope` controls shoaling, `swell_height_m` controls surf size, `period_s` controls crest frequency, and `peel` skews the moving break front. The sandy shore is ordinary static collision geometry; water and beach both use a zero-depth shoreline cutoff. This approximation does not yet render true plunging surf barrels.
