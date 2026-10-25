# Deep Explorer — Mini-Submarine

Generate a standalone mini-sub project using `dve_create_submarine NEW_PROJECT_DIRECTORY` or the editor's **Create mini-submarine** action.

Launch it with `dve_player`. The mini-sub begins in the same Gerstner ocean as the surface boats, above a seabed at approximately 40 metres.

W/S drive forward/back, A/D yaw, Q/E strafe, Space/Ctrl heave up/down, Z/X pump ballast out/in, Up/Down pitch, H toggles depth hold. P pauses, R restarts and F3 displays pressure/battery/depth details.

Try diving towards the wreck and colored markers at around 20–30 metres while holding depth and compensating for the water current. The ballast tank changes weight but never creates free thrust. Power is finite. The sea surface and water volume remain deterministic and fully headless-testable.

S1 introduces only *optical material coefficients*. S2 will handle the critical rendered experience: wavelength-selective sunlight filtering, extinction with view range, scattered in-light, above/below-surface transitions and submarine spotlights.
