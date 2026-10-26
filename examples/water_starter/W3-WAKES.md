# W3: Wake interaction in the watercraft harbor

The generated W2 harbor automatically enables W3 water disturbances. You do not need extra scene components.

1. Select **4 — Speedboat**, accelerate with W, and look behind its stern. The foam-highlighted waves and bow spray follow the boat's water-relative motion.
2. Select **5 — Wave Runner**, then cross the recent path of the speedboat. The hull's existing buoyancy probes receive the changed surface height, normal and velocity, including the newly created wake.
3. Select **2 — Pirate Ship**. Its large mass and 45-probe hull filter small chop differently than the 9-probe dinghy.
4. Press F3 for current wake count and local foam. Compare Calm, Breeze and Storm ocean presets on the Ocean Surface marker.

Spray droplets are cosmetic and never change boat velocity. The local wakes, unlike spray, are a real physical water disturbance. Saved games retain their wakes via DVE_WATER 3; old saves still load.
