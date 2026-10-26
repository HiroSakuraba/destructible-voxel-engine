# S2 — Underwater Color and Deep Explorer Lamps

Generate the Deep Explorer project using `dve_create_submarine NEW_PROJECT_DIRECTORY` or the editor's mini-sub starter. Launch in `dve_player`.

Submerge with Ctrl and approach the wreck at 20–30 metres. Red, green and blue survey markers demonstrate how distance through water and depth attenuate illumination differently. Twin forward-facing submarine spotlights illuminate and scatter light within their cones. Brightly colored nearby surfaces become visible under local illumination, while distant surfaces fade into water-column radiance. Surface crossing uses DVE's moving Gerstner water surface.

Select **Ocean Surface** in the editor to choose `water_clarity`: clear ocean (0), coastal (1), or turbid (2). Or turn on `custom_optics` and author `optical_absorption_rgb`, `optical_scattering_rgb`, and `optical_kd_rgb` independently (inverse metres). `sub_lamp_intensity` is on the vessel component.

S1 controls remain: W/S forward/reverse, A/D yaw, Q/E strafe, Space/Ctrl vertical movement, Z/X ballast, Up/Down pitch, H depth hold, F3 instruments.

**Current limitations:** The CPU renderer has physically based extinction and approximated in-scattering but no Snell geometry reray at the water surface, multiple scattering or calibrated 16-channel spectral reconstruction. Authoring values must be tuned to a specific water body for quantitative accuracy.
