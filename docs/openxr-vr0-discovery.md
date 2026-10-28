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
to a renderer, and submits an empty frame when rendering fails. An event pump handles session changes, instance loss, lost-event counts and reference-space
change records with their effective times and poses. It filters session-specific events
and uses a budget so the caller can drain remaining events before rendering. A stereo view
locator uses the predicted display time and rejects missing or invalid tracking.
The calling application must provide a valid instance, session, space and graphics binding,
invoke the pump and locator, manage image acquisition/release, and keep submitted layer
data alive through end-frame. Neither the probe nor the existing player connects them yet.

Release-build tests use fake callbacks and a fake OpenXR function table; they exercise
ordering, skipped rendering, errors, focus, stopping, restart, loss, event filtering,
recenter records and valid/invalid stereo view results. They do not
execute an OpenXR runtime or verify Vulkan synchronization.

## Vulkan and swapchain building blocks

With `DVE_BUILD_OPENXR_VULKAN_BRIDGE=ON`, also set `DVE_VULKAN_INCLUDE_DIR` to a
Vulkan-Headers `include` directory. Contract tests used Vulkan-Headers commit
`73814d006638aa00bd553215b054f0f02e457e22`. The bridge wraps the
`XR_KHR_vulkan_enable2` requirements, instance/device creation and runtime-selected GPU
calls; it can construct a graphics binding and enumerate runtime-owned Vulkan images.
The caller must enable the extension, resolve functions, choose a compatible Vulkan API
version and graphics queue, synchronize GPU work, and destroy owned Vulkan objects in
correct order. The bridge is not connected to DVE's VulkanDevice.

Session/swapchain wrappers create and explicitly close OpenXR handles. An acquired
swapchain image cannot be released until wait succeeds; timeout requires another wait.
The wrappers refuse to destroy an acquired swapchain, and callers must close handles
before their parent or runtime goes away. The projection helper produces right-handed,
negative-Z-forward eye projection with Vulkan depth [0,1] and positive-height viewport;
it does not convert DVE world-space axes or construct a view matrix.

The stereo-rendering coordinator now locates predicted-time views, acquires and waits
for two distinct per-eye swapchains, invokes a caller-supplied drawing callback, releases
the images, and assembles the OpenXR projection layer. The callback must bounds-check
the enumerated image index, perform Vulkan transitions and complete GPU work before
returning. A thrown callback or a wait/release failure poisons the coordinator when an
image may still be acquired; the application must stop and safely resolve the image
lifetime. Untracked poses produce no projection layer. The configuration helpers query
runtime-recommended stereo image sizes and pick from explicitly supported formats. The
fake-runtime tests exercise this end-to-end through the session controller, but no
production Vulkan drawing callback or real headset connection exists yet.

The dedicated `openxr-foundation.yml` workflow compiles these components and runs fake
API contract tests, not a native runtime or physical GPU/headset test.

## Remaining VR-0 work

- Test the CMake option and build on Windows and additional supported configurations.
  A Linux XR-enabled configure/build and fake-loader CTest passed; XR-disabled configure also passed.
- Integrate Vulkan ownership and the runtime-selected device into the actual player rendering path;
  the inspected player uses its CPU renderer and the Vulkan RHI manages its own device.
- Connect the event pump, a real created session and renderer to the controller;
  implement runtime-owned swapchain image interop,
  predicted-time stereo views, coordinate conversions and synchronization.
- Build an actual stereo diagnostic scene and validate it with an executing OpenXR
  runtime and physical headset; record device, runtime and GPU. No such run has been made.
- Preserve normal non-XR rendering and exercise error cleanup and focus transitions.

Reference: https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr
and https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html
