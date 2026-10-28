# VR-0: experimental OpenXR runtime discovery

This is an early part of VR-0, not a VR player. The optional `dve_openxr_probe`
loads an OpenXR loader, enumerates instance extensions, creates an instance, prints
runtime information, and requests an HMD system. It reports whether either Vulkan
extension is advertised. It does not enable the extensions, query Vulkan requirements,
create a session, enumerate swapchains, render stereo, or display anything in a headset.
Extension advertisement alone is not proof of graphics compatibility.

## Build

Use the Khronos OpenXR-SDK headers from a known checkout. Development used SDK source
commit `24a1f2b92d212501a2b1b8d30ffd53abf55fcfa8`. With an existing SDK checkout:

```
cmake -S . -B build-xr -DDVE_BUILD_OPENXR_PROBE=ON -DDVE_OPENXR_INCLUDE_DIR=/path/to/OpenXR-SDK/include
cmake --build build-xr --target dve_openxr_probe
```

The option defaults OFF. No SDK or loader is required for existing builds. The probe
loads `openxr_loader.dll` on Windows or `libopenxr_loader.so.1` on Linux; a specific
loader can be selected with `--loader /path/to/loader`. Only use a trusted loader.
Configure the active OpenXR runtime and attach a headset before running the probe.
Absence of a runtime, loader or headset produces an error and a nonzero exit code.

On Unix with tests and Python available, build `dve_fake_openxr_loader` and run
`ctest --test-dir build-xr -R '^dve_openxr_probe_tests$' --output-on-failure`.
The shim checks success, missing headset, missing loader and bad arguments, including
instance destruction. It is not a runtime, Vulkan, session, graphics or hardware test.

## Session and frame controller

The optional `dve_xr_session_loop` library contains a session-state controller and an
OpenXR driver around caller-provided function pointers and an existing session. It begins
and ends a session on state events, honors `shouldRender`, passes predicted display time
to a renderer, and submits an empty frame when rendering fails. The calling application
must poll OpenXR events and map session states, provide a valid session and graphics
binding, manage image acquisition/release, and keep submitted layer data alive through
end-frame. Neither the probe nor the existing player connects this controller yet.

Release-build tests use fake callbacks and a fake OpenXR function table; they exercise
ordering, skipped rendering, errors, focus, stopping, restart and loss. They do not
execute an OpenXR runtime or verify Vulkan synchronization.

## Remaining VR-0 work

- Test the CMake option and build on Windows and additional supported configurations.
  A Linux XR-enabled configure/build and fake-loader CTest passed; XR-disabled configure also passed.
- Audit the active game-rendering path and design Vulkan ownership and required-device selection.
- Connect runtime event polling, a real created session and renderer to the controller;
  implement runtime-owned swapchain image interop,
  predicted-time stereo views, coordinate conversions and synchronization.
- Build an actual stereo diagnostic scene and validate it with an executing OpenXR
  runtime and physical headset; record device, runtime and GPU. No such run has been made.
- Preserve normal non-XR rendering and exercise error cleanup and focus transitions.

Reference: https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr
and https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
