cmake_minimum_required(VERSION 3.24)
project(destructible_voxel_engine_core VERSION 2.35.1 LANGUAGES CXX)

option(DVE_BUILD_TESTS "Build the headless correctness tests" ON)
option(DVE_BUILD_BENCHMARK "Build the headless benchmark" ON)
option(DVE_BUILD_ASSET_COOKER "Build the offline model-to-voxel asset cooker" ON)
option(DVE_BUILD_ASSET_PIPELINE_TESTS "Build PNG/JPEG asset-pipeline tests when tests are enabled" ON)
option(DVE_BUILD_EDITOR "Build the human-centered editor foundation and demo" ON)
option(DVE_BUILD_NATIVE_EDITOR "Build the native desktop editor shell when a supported window system is available" ON)
option(DVE_BUILD_PLATFORM "Build the shared application-host abstraction" ON)
option(DVE_BUILD_RHI "Build the Render Hardware Interface and validation backend" ON)
option(DVE_BUILD_DEVICE_TEST_RUNNER "Build the headless simulation/device evidence runner" ON)
option(DVE_BUILD_AUDIO_SYNTH "Build the synthesizer, MIDI, game-audio mixer, events, and acoustics" ON)
option(DVE_ENABLE_RTMIDI "Enable native MIDI input/output when RtMidi is available" ON)
option(DVE_ENABLE_FFMPEG_CLI_IMPORT "Enable broad authoring-time media import through an installed FFmpeg executable" ON)
option(DVE_ENABLE_STEAM_AUDIO "Build the native Steam Audio HRTF adapter when the SDK is available" ON)
set(DVE_STEAM_AUDIO_ROOT "" CACHE PATH "Steam Audio SDK root containing include/phonon.h and platform libraries")
option(DVE_FETCH_RTMIDI "Fetch the pinned RtMidi 6.0.0 source when no installed package is available" OFF)
option(DVE_BUILD_LEGACY_X11_HOST "Retain the v1.14 X11 host as a comparison oracle during migration" ON)
option(DVE_ENABLE_SDL3_HOST "Build the shared SDL3 desktop host when SDL3 is available" ON)
option(DVE_FETCH_SDL3 "Fetch the pinned SDL 3.4.12 source when no installed package is available" OFF)
option(DVE_BUILD_SDL3_CONTRACT_TESTS "Compile and execute SDL host/editor logic against the deterministic SDL API shim" ON)
option(DVE_ENABLE_VULKAN_BUFFER_BACKEND "Build the dynamically loaded Vulkan offscreen graphics backend" ON)
option(DVE_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DVE_ENABLE_FLUODDITY "Build the native Fluoddity rule, preset, and simulation-contract module" ON)
option(DVE_ENABLE_SOFT_BODIES "Build native cloth, rope, vegetation, and deformable soft-body foundations" ON)
option(DVE_ENABLE_CPU_HAIR "Build the batched multithreaded CPU guide-hair runtime and Sisir PLY importer" ON)
option(DVE_ENABLE_VFX_PARTICLES "Build the authored visual-effects particle graph and runtime" ON)
option(DVE_ENABLE_PBF_LIQUID "Build the bounded PBF/XPBD liquid prototype" ON)
option(DVE_ENABLE_SIMULATION_ENERGY_COMPILER "Build the native relational simulation-energy compiler" ON)
option(DVE_ENABLE_BSPLINE_CLOTH "Build the quadratic B-spline cloth proxy and FEM foundation" ON)
option(DVE_ENABLE_GRID_FLUIDS "Build the native MAC-grid smoke, fire, and FLIP-transfer foundation" ON)
option(DVE_ENABLE_FLIP_LIQUIDS "Build the free-surface FLIP/APIC liquid foundation" ON)
option(DVE_ENABLE_BOX2D "Build the optional Box2D 3.1+ sprite/2D rigid-body backend" OFF)
option(DVE_FETCH_BOX2D "Fetch the pinned supported Box2D source when no installed/source target is supplied" OFF)
option(DVE_BUILD_BOX2D_CONTRACT_TESTS "Compile the Box2D adapter against a deterministic API contract shim" ON)
set(DVE_BOX2D_SOURCE_DIR "" CACHE PATH "Path to a Box2D 3.1+ source checkout")
set(BOX2D_INCLUDE_DIR "" CACHE PATH "Legacy: Box2D include root containing box2d/box2d.h")
set(BOX2D_LIBRARY "" CACHE FILEPATH "Legacy: built Box2D library")
set(DVE_BOX2D_LEGACY_VERSION "3.1.1" CACHE STRING "Version of a legacy Box2D include/library pair")
option(DVE_ENABLE_BOX3D "Build the optional Box3D 0.1.x three-dimensional rigid-body backend" OFF)
option(DVE_FETCH_BOX3D "Fetch the pinned Box3D source when no installed/source target is supplied" OFF)
set(DVE_BOX3D_SOURCE_DIR "" CACHE PATH "Path to a Box3D 0.1.x source checkout")
set(BOX3D_INCLUDE_DIR "" CACHE PATH "Legacy: Box3D include root containing box3d/box3d.h")
set(BOX3D_LIBRARY "" CACHE FILEPATH "Legacy: built Box3D library")
set(DVE_BOX3D_LEGACY_VERSION "0.1.0" CACHE STRING "Version of a legacy Box3D include/library pair")
option(DVE_ENABLE_JOLT "Build the Jolt Physics rigid-body backend (WP-4.4)" OFF)
option(DVE_BUILD_JOLT_SMOKE "Build native Jolt rigid-body integration smoke tests" ON)
option(DVE_ENABLE_JOLT_DEBUG_DRAW "Compile Jolt debug-render capture into the DVE backend" ON)
option(DVE_ENABLE_LUA "Build the Lua 5.4 gameplay scripting binding" OFF)
option(DVE_FETCH_JOLT "Fetch the pinned Jolt v5.6.0 source when no installed/source target is supplied" OFF)
set(DVE_JOLT_SOURCE_DIR "" CACHE PATH "Path to a JoltPhysics 5.5.x-5.6.x source checkout")
set(JOLT_INCLUDE_DIR "" CACHE PATH "Legacy: Jolt checkout root containing Jolt/Jolt.h")
set(JOLT_LIBRARY "" CACHE FILEPATH "Legacy: built libJolt.a or Jolt.lib")
set(DVE_JOLT_LEGACY_VERSION "" CACHE STRING "Required version for legacy Jolt include/library pairs (supported: 5.5.x-5.6.x)")

# Geometry product profile. HYBRID is the general-purpose build: destructible voxel objects and
# ordinary indexed polygon meshes coexist and share materials, transforms, scripting, physics,
# and packaging. VOXEL and POLYGON profiles reject publication of the other geometry kind while
# retaining source compatibility for tools and common data structures.
set(DVE_GEOMETRY_MODE "HYBRID" CACHE STRING "Geometry profile: VOXEL, POLYGON, or HYBRID")
set_property(CACHE DVE_GEOMETRY_MODE PROPERTY STRINGS VOXEL POLYGON HYBRID)
string(TOUPPER "${DVE_GEOMETRY_MODE}" DVE_GEOMETRY_MODE_NORMALIZED)
if(NOT DVE_GEOMETRY_MODE_NORMALIZED STREQUAL "VOXEL" AND
   NOT DVE_GEOMETRY_MODE_NORMALIZED STREQUAL "POLYGON" AND
   NOT DVE_GEOMETRY_MODE_NORMALIZED STREQUAL "HYBRID")
    message(FATAL_ERROR "DVE_GEOMETRY_MODE must be VOXEL, POLYGON, or HYBRID")
endif()
message(STATUS "DVE geometry profile: ${DVE_GEOMETRY_MODE_NORMALIZED}")

add_library(dve_core
    src/brick.cpp
    src/voxel_object.cpp
    src/damage.cpp
    src/destruction_material.cpp
    src/surface.cpp
    src/connectivity.cpp
    src/query.cpp
    src/transform.cpp
    src/animation.cpp
    src/animation_controller.cpp
    src/ragdoll_runtime.cpp
    src/marionette_runtime.cpp
    src/control_rig.cpp
    src/control_rig_authoring.cpp
    src/component.cpp
    src/camera_system.cpp
    src/camera_runtime.cpp
    src/camera_sequence.cpp
    src/collision_proxy.cpp
    src/fragment.cpp
    src/work_debt.cpp
    src/job_system.cpp
    src/derived_jobs.cpp
    src/gpu_brick_mirror.cpp
    src/packed_brickmap.cpp
    src/rigid_body_adapter.cpp
    src/physics3d_backend.cpp
    src/sprite2d.cpp
    src/display_presentation.cpp
    src/local_multiplayer_presentation.cpp
    src/sprite_animation.cpp
    src/sprite_rig2d.cpp
    src/sprite_diagnostics.cpp
    src/render3d_diagnostics.cpp
    src/voxel_material_policy.cpp
    src/brick_palette.cpp
    src/brick_palette_asset_cooker.cpp
    src/render/brick_palette_shading.cpp
    src/voxel_material_switcher.cpp
    src/sprite_palette.cpp
    src/sprite_authoring.cpp
    src/tilemap2d.cpp
    src/tilemap_authoring.cpp
    src/physics2d.cpp
    src/character_controller2d.cpp
    src/game_world.cpp
    src/gameplay_runtime.cpp
    src/replication_contract.cpp
    src/network_simulation.cpp
    src/network_crypto.cpp
    src/network_udp.cpp
    src/network_runtime_services.cpp
    src/game_ui.cpp
    src/game_ui_asset.cpp
    src/game_ui_localization.cpp
    src/material_parameter_collection.cpp
    src/material_mapping.cpp
    src/material_layers.cpp
    src/material_decal.cpp
    src/material_displacement.cpp
    src/material_authoring.cpp
    src/environment_lighting.cpp
    src/environment_lighting_asset.cpp
    src/gpu_material_parameter_collection.cpp
    src/master_material.cpp
    src/gpu_material_buffer.cpp
    src/gpu_material_mapping.cpp
    src/render_environment.cpp
    src/gpu_render_environment.cpp
    src/dvox.cpp
    src/matrix4.cpp
    src/polygon_asset.cpp
    src/text3d.cpp
    src/gabor_volume.cpp
    src/runtime_mesh_world.cpp
    src/runtime_text3d_world.cpp
    src/runtime_gabor_volume_world.cpp
    src/hybrid_geometry_world.cpp
    src/polygon_collision.cpp
    src/polygon_bvh.cpp
    src/hybrid_scene_manifest.cpp
    src/runtime_scene.cpp
    src/navigation_mesh.cpp
    src/navigation_geometry.cpp
    src/editable_mesh.cpp
    src/geometry_graph.cpp
    src/runtime_brickmap_world.cpp
    src/persistent_byte_heap.cpp
    src/ai/json.cpp
    src/ai/assistant.cpp
    src/ai/project_patch.cpp
    src/ai/named_tasks.cpp
    src/ai/mcp.cpp
    src/ai/live_editor_mcp.cpp
    src/ai/openai_responses.cpp
    src/editor_ai_assistant.cpp
    src/editor_tasks.cpp
    src/simulation_quality.cpp
    src/simulation_validation.cpp
    src/v235_foundations.cpp
)
add_library(dve::core ALIAS dve_core)
if(DVE_ENABLE_FLUODDITY)
    target_sources(dve_core PRIVATE src/fluoddity.cpp src/fluoddity_solver.cpp src/runtime_fluoddity_world.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_FLUODDITY=1)
endif()
if(DVE_ENABLE_SOFT_BODIES)
    target_sources(dve_core PRIVATE src/soft_body.cpp src/soft_body_asset.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_SOFT_BODIES=1)
endif()
if(DVE_ENABLE_CPU_HAIR)
    target_sources(dve_core PRIVATE src/cpu_hair.cpp src/cpu_hair_runtime.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_CPU_HAIR=1)
endif()
if(DVE_ENABLE_VFX_PARTICLES)
    target_sources(dve_core PRIVATE src/vfx_particle_graph.cpp src/sprite_particle_renderer.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_VFX_PARTICLES=1)
endif()
if(DVE_ENABLE_PBF_LIQUID)
    target_sources(dve_core PRIVATE src/liquid_pbf.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_PBF_LIQUID=1)
endif()
if(DVE_ENABLE_SIMULATION_ENERGY_COMPILER)
    target_sources(dve_core PRIVATE src/simulation_energy.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_SIMULATION_ENERGY_COMPILER=1)
endif()
if(DVE_ENABLE_BSPLINE_CLOTH)
    target_sources(dve_core PRIVATE src/bspline_cloth.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_BSPLINE_CLOTH=1)
endif()
if(DVE_ENABLE_SOFT_BODIES AND DVE_ENABLE_BSPLINE_CLOTH)
    target_sources(dve_core PRIVATE src/deformable_runtime.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_DEFORMABLE_RUNTIME=1)
endif()
if(DVE_ENABLE_GRID_FLUIDS)
    target_sources(dve_core PRIVATE src/grid_fluid.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_GRID_FLUIDS=1)
endif()
if(DVE_ENABLE_FLIP_LIQUIDS)
    if(NOT DVE_ENABLE_GRID_FLUIDS)
        message(FATAL_ERROR "DVE_ENABLE_FLIP_LIQUIDS requires DVE_ENABLE_GRID_FLUIDS")
    endif()
    target_sources(dve_core PRIVATE src/flip_liquid.cpp)
    target_compile_definitions(dve_core PUBLIC DVE_ENABLE_FLIP_LIQUIDS=1)
endif()
if(DVE_GEOMETRY_MODE_NORMALIZED STREQUAL "VOXEL")
    target_compile_definitions(dve_core PUBLIC DVE_GEOMETRY_MODE_VOXEL=1)
elseif(DVE_GEOMETRY_MODE_NORMALIZED STREQUAL "POLYGON")
    target_compile_definitions(dve_core PUBLIC DVE_GEOMETRY_MODE_POLYGON=1)
else()
    target_compile_definitions(dve_core PUBLIC DVE_GEOMETRY_MODE_HYBRID=1)
endif()

add_executable(dve_material_system_demo apps/material_system_demo.cpp)
target_link_libraries(dve_material_system_demo PRIVATE dve::core)
add_executable(dve_pack apps/dve_pack.cpp)
target_link_libraries(dve_pack PRIVATE dve::core)
add_executable(dve_material_audit apps/material_audit_v174.cpp)
target_link_libraries(dve_material_audit PRIVATE dve::core)
add_executable(dve_generate_sprite_assets_v218 apps/generate_sprite_assets_v218.cpp)
target_link_libraries(dve_generate_sprite_assets_v218 PRIVATE dve::core)
add_executable(dve_display_presentation_v221_demo apps/display_presentation_v221_demo.cpp)
target_link_libraries(dve_display_presentation_v221_demo PRIVATE dve::core)
add_executable(dve_render3d_diagnostics_v223_demo apps/render3d_diagnostics_v223_demo.cpp)
target_link_libraries(dve_render3d_diagnostics_v223_demo PRIVATE dve::core)
target_compile_definitions(dve_render3d_diagnostics_v223_demo PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(dve_voxel_material_policy_v224_demo apps/voxel_material_policy_v224_demo.cpp)
target_link_libraries(dve_voxel_material_policy_v224_demo PRIVATE dve::core)
target_compile_definitions(dve_voxel_material_policy_v224_demo PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_executable(dve_cook_text3d apps/dve_cook_text3d.cpp)
target_link_libraries(dve_cook_text3d PRIVATE dve::core)
add_executable(dve_cook_gabor apps/dve_cook_gabor.cpp)
target_link_libraries(dve_cook_gabor PRIVATE dve::core)
if(DVE_ENABLE_FLUODDITY)
    add_executable(dve_cook_fluoddity apps/dve_cook_fluoddity.cpp)
    target_link_libraries(dve_cook_fluoddity PRIVATE dve::core)
endif()
if(DVE_ENABLE_CPU_HAIR)
    add_executable(dve_cook_hair apps/dve_cook_hair.cpp)
    target_link_libraries(dve_cook_hair PRIVATE dve::core)
endif()

add_executable(dve_mcp_server apps/dve_mcp_server.cpp)
target_link_libraries(dve_mcp_server PRIVATE dve::core)
add_executable(dve_live_editor_mcp_proxy apps/dve_live_editor_mcp_proxy.cpp)
target_link_libraries(dve_live_editor_mcp_proxy PRIVATE dve::core)
add_executable(dve_ai_assistant_demo apps/ai_assistant_v139_demo.cpp)
target_link_libraries(dve_ai_assistant_demo PRIVATE dve::core)
add_executable(dve_playable_runtime_v145_demo apps/playable_runtime_v145_demo.cpp)
target_link_libraries(dve_playable_runtime_v145_demo PRIVATE dve::core)
add_executable(dve_network_simulation_v146_demo apps/network_simulation_v146_demo.cpp)
target_link_libraries(dve_network_simulation_v146_demo PRIVATE dve::core)
add_executable(dve_network_runtime_v147_demo apps/network_runtime_v147_demo.cpp)
target_link_libraries(dve_network_runtime_v147_demo PRIVATE dve::core)
add_executable(dve_network_transport_v148_demo apps/network_transport_v148_demo.cpp)
target_link_libraries(dve_network_transport_v148_demo PRIVATE dve::core)

find_package(Threads REQUIRED)
target_link_libraries(dve_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(dve_core PUBLIC ws2_32)
endif()

if(DVE_BUILD_AUDIO_SYNTH)
    add_library(dve_audio_synth
        src/audio/midi.cpp
        src/audio/sample_map.cpp
        src/audio/sample_map_recipe.cpp
        src/audio/synthesizer.cpp
        src/audio/chiptune.cpp
        src/audio/chiptune_authoring.cpp
        src/audio/spatializer.cpp
        src/audio/ambisonics.cpp
        src/audio/spatial_transition.cpp
        src/audio/mixer.cpp
        src/audio/event_graph.cpp
        src/audio/destruction_audio.cpp
        src/audio/acoustics.cpp
        src/audio/audio_asset.cpp
        src/audio/audio_edit.cpp
        src/audio/audio_analysis.cpp
        src/audio/audio_recovery.cpp
        src/audio/interactive_music.cpp
        src/audio/audio_recorder.cpp
        src/audio/audio_device_capture.cpp
        src/audio/audio_metering.cpp
        src/audio/audio_repair.cpp
        src/audio/audio_time_stretch.cpp
        src/audio/audio_effects.cpp
        src/audio/clap_host.cpp
        src/audio/destruction_runtime.cpp
        src/audio/destruction_ingress.cpp
        src/audio/destruction_commit_coordinator.cpp
        src/audio/physics_contact_audio_accumulator.cpp
        src/audio/physics_contact_sink_adapter.cpp
        src/audio/reflection_runtime.cpp
        src/audio/acoustic_runtime.cpp
        src/audio/steam_audio.cpp)
    add_library(dve::audio_synth ALIAS dve_audio_synth)
    target_include_directories(dve_audio_synth PUBLIC include)
    target_compile_features(dve_audio_synth PUBLIC cxx_std_23)
    target_link_libraries(dve_audio_synth PUBLIC Threads::Threads)
    if(MSVC)
        target_compile_options(dve_audio_synth PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_audio_synth PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_audio_synth PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_audio_synth PUBLIC -fsanitize=address,undefined)
    endif()

    find_library(DVE_SNDFILE_LIBRARY NAMES sndfile libsndfile)
    if(NOT DVE_SNDFILE_LIBRARY AND UNIX)
        find_file(DVE_SNDFILE_RUNTIME_LIBRARY NAMES libsndfile.so.1 libsndfile.dylib
            PATHS /lib /usr/lib /usr/local/lib
            PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu)
        if(DVE_SNDFILE_RUNTIME_LIBRARY)
            set(DVE_SNDFILE_LIBRARY "${DVE_SNDFILE_RUNTIME_LIBRARY}")
        endif()
    endif()
    if(DVE_SNDFILE_LIBRARY)
        target_sources(dve_audio_synth PRIVATE src/audio/sndfile_decoder.cpp)
        target_link_libraries(dve_audio_synth PRIVATE ${DVE_SNDFILE_LIBRARY})
        target_compile_definitions(dve_audio_synth PRIVATE DVE_HAVE_SNDFILE=1)
        message(STATUS "libsndfile found: FLAC/Ogg/WAV import enabled")
    else()
        message(STATUS "libsndfile not found: native WAV import remains available")
    endif()

    if(DVE_ENABLE_FFMPEG_CLI_IMPORT)
        find_program(DVE_FFMPEG_EXECUTABLE NAMES ffmpeg)
        if(DVE_FFMPEG_EXECUTABLE)
            target_sources(dve_audio_synth PRIVATE src/audio/ffmpeg_cli_decoder.cpp)
            target_compile_definitions(dve_audio_synth PRIVATE
                DVE_HAVE_FFMPEG_CLI=1
                DVE_FFMPEG_EXECUTABLE_PATH="${DVE_FFMPEG_EXECUTABLE}")
            message(STATUS "FFmpeg found: MP3/MP4/M4A/AAC/Opus/WebM and broad media import enabled")
        else()
            message(STATUS "FFmpeg not found: native WAV and optional libsndfile import remain available")
        endif()
    endif()

    add_executable(dve_cook_audio apps/dve_cook_audio.cpp)
    target_link_libraries(dve_cook_audio PRIVATE dve::audio_synth)
    add_executable(dve_cook_chiptune apps/dve_cook_chiptune.cpp)
    target_link_libraries(dve_cook_chiptune PRIVATE dve::audio_synth)
    add_executable(dve_chiptune_tests tests/test_chiptune.cpp)
    target_link_libraries(dve_chiptune_tests PRIVATE dve::audio_synth)
    add_test(NAME dve_chiptune_tests COMMAND dve_chiptune_tests)
    add_executable(dve_chiptune_authoring_tests tests/test_chiptune_authoring_v217.cpp)
    target_link_libraries(dve_chiptune_authoring_tests PRIVATE dve::audio_synth)
    add_test(NAME dve_chiptune_authoring_tests COMMAND dve_chiptune_authoring_tests)

    add_library(dve_chiptune_vertical_slice src/chiptune_vertical_slice.cpp)
    add_library(dve::chiptune_vertical_slice ALIAS dve_chiptune_vertical_slice)
    target_include_directories(dve_chiptune_vertical_slice PUBLIC include)
    target_compile_features(dve_chiptune_vertical_slice PUBLIC cxx_std_23)
    target_link_libraries(dve_chiptune_vertical_slice PUBLIC dve::core dve::audio_synth)
    if(MSVC)
        target_compile_options(dve_chiptune_vertical_slice PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_chiptune_vertical_slice PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_chiptune_vertical_slice PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_chiptune_vertical_slice PUBLIC -fsanitize=address,undefined)
    endif()
    add_executable(dve_chiptune_vertical_slice_tests tests/test_chiptune_vertical_slice_v217.cpp)
    target_link_libraries(dve_chiptune_vertical_slice_tests PRIVATE dve::chiptune_vertical_slice)
    target_compile_definitions(dve_chiptune_vertical_slice_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_chiptune_vertical_slice_tests COMMAND dve_chiptune_vertical_slice_tests)
    add_executable(dve_chiptune_vertical_slice_demo apps/chiptune_vertical_slice_v217_demo.cpp)
    target_link_libraries(dve_chiptune_vertical_slice_demo PRIVATE dve::chiptune_vertical_slice)
    target_compile_definitions(dve_chiptune_vertical_slice_demo PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(DVE_ENABLE_VFX_PARTICLES)
        add_library(dve_sprite_vertical_slice_presentation src/sprite_vertical_slice_presentation.cpp)
        add_library(dve::sprite_vertical_slice_presentation ALIAS dve_sprite_vertical_slice_presentation)
        target_include_directories(dve_sprite_vertical_slice_presentation PUBLIC include)
        target_compile_features(dve_sprite_vertical_slice_presentation PUBLIC cxx_std_23)
        target_link_libraries(dve_sprite_vertical_slice_presentation PUBLIC dve::chiptune_vertical_slice dve::core)
        if(MSVC)
            target_compile_options(dve_sprite_vertical_slice_presentation PRIVATE /W4 /permissive-)
        else()
            target_compile_options(dve_sprite_vertical_slice_presentation PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        endif()
        if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
            target_compile_options(dve_sprite_vertical_slice_presentation PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(dve_sprite_vertical_slice_presentation PUBLIC -fsanitize=address,undefined)
        endif()
        add_executable(dve_sprite_vertical_slice_v218_demo apps/sprite_vertical_slice_v218_demo.cpp)
        target_link_libraries(dve_sprite_vertical_slice_v218_demo PRIVATE dve::sprite_vertical_slice_presentation)
        target_compile_definitions(dve_sprite_vertical_slice_v218_demo PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    add_executable(dve_cook_sample_map apps/dve_cook_sample_map.cpp)
    target_link_libraries(dve_cook_sample_map PRIVATE dve::audio_synth)
    add_executable(dve_wall_collapse_audio_demo apps/wall_collapse_audio_demo.cpp)
    target_link_libraries(dve_wall_collapse_audio_demo PRIVATE dve::audio_synth)
    add_executable(dve_audio_edit_demo apps/audio_edit_demo.cpp)
    target_link_libraries(dve_audio_edit_demo PRIVATE dve::audio_synth)

    if(DVE_ENABLE_RTMIDI)
        set(DVE_RTMIDI_TARGET "")
        find_package(RtMidi 6 CONFIG QUIET)
        if(TARGET RtMidi::rtmidi)
            set(DVE_RTMIDI_TARGET RtMidi::rtmidi)
        elseif(TARGET rtmidi)
            set(DVE_RTMIDI_TARGET rtmidi)
        endif()

        if(NOT DVE_RTMIDI_TARGET)
            find_path(RTMIDI_INCLUDE_DIR RtMidi.h PATH_SUFFIXES rtmidi)
            find_library(RTMIDI_LIBRARY NAMES rtmidi RtMidi)
            if(RTMIDI_INCLUDE_DIR AND RTMIDI_LIBRARY)
                add_library(dve_rtmidi_imported UNKNOWN IMPORTED)
                set_target_properties(dve_rtmidi_imported PROPERTIES
                    IMPORTED_LOCATION "${RTMIDI_LIBRARY}"
                    INTERFACE_INCLUDE_DIRECTORIES "${RTMIDI_INCLUDE_DIR}")
                set(DVE_RTMIDI_TARGET dve_rtmidi_imported)
            endif()
        endif()

        if(NOT DVE_RTMIDI_TARGET AND DVE_FETCH_RTMIDI)
            include(FetchContent)
            set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
            set(RTMIDI_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
            set(RTMIDI_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
            set(RTMIDI_BUILD_MODULES OFF CACHE BOOL "" FORCE)
            FetchContent_Declare(RtMidi
                GIT_REPOSITORY https://github.com/thestk/rtmidi.git
                GIT_TAG 6.0.0
                GIT_SHALLOW TRUE)
            FetchContent_MakeAvailable(RtMidi)
            if(TARGET rtmidi)
                set(DVE_RTMIDI_TARGET rtmidi)
            endif()
        endif()

        if(DVE_RTMIDI_TARGET)
            target_sources(dve_audio_synth PRIVATE src/audio/rtmidi_backend.cpp)
            target_link_libraries(dve_audio_synth PRIVATE ${DVE_RTMIDI_TARGET})
            target_compile_definitions(dve_audio_synth PRIVATE DVE_HAVE_RTMIDI=1)
            message(STATUS "RtMidi found: native MIDI input/output enabled")
        else()
            message(STATUS "RtMidi not found: deterministic virtual MIDI remains available")
        endif()
    endif()

    if(DVE_ENABLE_STEAM_AUDIO)
        find_path(DVE_STEAM_AUDIO_INCLUDE_DIR phonon.h
            HINTS "${DVE_STEAM_AUDIO_ROOT}" "${DVE_STEAM_AUDIO_ROOT}/include"
            PATH_SUFFIXES include)
        find_library(DVE_STEAM_AUDIO_LIBRARY NAMES phonon steamaudio
            HINTS "${DVE_STEAM_AUDIO_ROOT}" "${DVE_STEAM_AUDIO_ROOT}/lib"
            PATH_SUFFIXES lib lib/linux-x64 lib/windows-x64 lib/osx)
        if(DVE_STEAM_AUDIO_INCLUDE_DIR AND DVE_STEAM_AUDIO_LIBRARY)
            add_library(dve_audio_spatial_steam src/audio/steam_audio_native.cpp)
            add_library(dve::audio_spatial_steam ALIAS dve_audio_spatial_steam)
            target_include_directories(dve_audio_spatial_steam PUBLIC include PRIVATE ${DVE_STEAM_AUDIO_INCLUDE_DIR})
            target_compile_features(dve_audio_spatial_steam PUBLIC cxx_std_23)
            target_link_libraries(dve_audio_spatial_steam PUBLIC dve::audio_synth PRIVATE ${DVE_STEAM_AUDIO_LIBRARY})
            if(MSVC)
                target_compile_options(dve_audio_spatial_steam PRIVATE /W4 /permissive-)
            else()
                target_compile_options(dve_audio_spatial_steam PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
            endif()
            message(STATUS "Steam Audio SDK found: native HRTF adapter enabled")
        else()
            message(STATUS "Steam Audio SDK not found: neutral contract and fake-ABI tests remain enabled")
        endif()
    endif()
endif()

if(DVE_BUILD_PLATFORM)
    add_library(dve_platform
        src/platform/application_host.cpp
        src/platform/headless_application_host.cpp)
    add_library(dve::platform ALIAS dve_platform)
    target_include_directories(dve_platform PUBLIC include)
    target_compile_features(dve_platform PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(dve_platform PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_platform PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_platform PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_platform PUBLIC -fsanitize=address,undefined)
    endif()
endif()

# SDL 3.4.12 is the pinned portability dependency. Installed packages are preferred; source
# fetching is explicit so offline and distro builds remain deterministic.
set(DVE_SDL3_TARGET "")
if(DVE_BUILD_PLATFORM AND DVE_ENABLE_SDL3_HOST)
    find_package(SDL3 3.2 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        set(DVE_SDL3_TARGET SDL3::SDL3)
    elseif(DVE_FETCH_SDL3)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(SDL3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-3.4.12.tar.gz
            URL_HASH SHA256=f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7)
        FetchContent_MakeAvailable(SDL3)
        if(TARGET SDL3::SDL3)
            set(DVE_SDL3_TARGET SDL3::SDL3)
        elseif(TARGET SDL3::SDL3-static)
            set(DVE_SDL3_TARGET SDL3::SDL3-static)
        endif()
    endif()

    if(DVE_SDL3_TARGET)
        add_library(dve_platform_sdl3 src/platform/sdl_application_host.cpp)
        add_library(dve::platform_sdl3 ALIAS dve_platform_sdl3)
        target_include_directories(dve_platform_sdl3 PUBLIC include)
        target_compile_features(dve_platform_sdl3 PUBLIC cxx_std_23)
        target_link_libraries(dve_platform_sdl3 PUBLIC dve::platform ${DVE_SDL3_TARGET})
        if(MSVC)
            target_compile_options(dve_platform_sdl3 PRIVATE /W4 /permissive-)
        else()
            target_compile_options(dve_platform_sdl3 PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        endif()
    else()
        message(STATUS "SDL3 3.2+ was not found; dve_desktop_editor is disabled (contract shim remains available to tests)")
    endif()
endif()

if(DVE_BUILD_RHI)
    add_library(dve_rhi
        src/rhi/device.cpp
        src/rhi/null_device.cpp)
    add_library(dve::rhi ALIAS dve_rhi)
    target_include_directories(dve_rhi PUBLIC include)
    target_compile_features(dve_rhi PUBLIC cxx_std_23)
    if(MSVC)
        target_compile_options(dve_rhi PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_rhi PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_rhi PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_rhi PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(DVE_BUILD_RHI AND DVE_ENABLE_VULKAN_BUFFER_BACKEND)
    add_library(dve_rhi_vulkan src/rhi/vulkan_device.cpp)
    add_library(dve::rhi_vulkan ALIAS dve_rhi_vulkan)
    target_include_directories(dve_rhi_vulkan PUBLIC include PRIVATE src/rhi)
    target_compile_features(dve_rhi_vulkan PUBLIC cxx_std_23)
    target_link_libraries(dve_rhi_vulkan PUBLIC dve::rhi PRIVATE ${CMAKE_DL_LIBS})
    if(MSVC)
        target_compile_options(dve_rhi_vulkan PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_rhi_vulkan PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_rhi_vulkan PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_rhi_vulkan PUBLIC -fsanitize=address,undefined)
    endif()
endif()

if(DVE_BUILD_DEVICE_TEST_RUNNER AND DVE_BUILD_RHI)
    add_library(dve_simulation_device_validation
        src/simulation_kernel_conformance.cpp
        src/simulation_production_conformance.cpp)
    add_library(dve::simulation_device_validation ALIAS dve_simulation_device_validation)
    target_include_directories(dve_simulation_device_validation PUBLIC include)
    target_compile_features(dve_simulation_device_validation PUBLIC cxx_std_23)
    target_link_libraries(dve_simulation_device_validation PUBLIC dve::core dve::rhi)
    if(MSVC)
        target_compile_options(dve_simulation_device_validation PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_simulation_device_validation PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_simulation_device_validation PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_simulation_device_validation PUBLIC -fsanitize=address,undefined)
    endif()

    add_executable(dve_device_test_runner apps/dve_device_test_runner.cpp)
    target_link_libraries(dve_device_test_runner PRIVATE dve::simulation_device_validation)
    target_compile_definitions(dve_device_test_runner PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(TARGET dve_rhi_vulkan)
        target_link_libraries(dve_device_test_runner PRIVATE dve::rhi_vulkan)
        target_compile_definitions(dve_device_test_runner PRIVATE DVE_DEVICE_RUNNER_HAS_VULKAN=1)
    endif()
endif()

if(DVE_BUILD_RHI)
    add_library(dve_render_bridge
        src/render/brick_palette_rhi.cpp
        src/render/brick_palette_gpu_resolve.cpp
        src/render/brickmap_rhi_mirror.cpp
        src/render/mesh_rhi_mirror.cpp
        src/render/mesh_heap.cpp
        src/render/polygon_renderer.cpp
        src/render/sprite_renderer.cpp
        src/render/texture_residency.cpp
        src/render/texture_preview.cpp
        src/render/text3d_renderer.cpp
        src/render/gabor_volume_renderer.cpp
        src/render/camera_frame_graph.cpp
        src/render/multi_surface_presenter.cpp
        src/render/voxel_lighting_plan.cpp
        src/render/cascaded_shadow_map.cpp
        src/render/cascaded_shadow_atlas.cpp
        src/render/environment_lighting_gpu.cpp
        src/render/ray_lighting_reference.cpp
        src/render/live_environment_renderer.cpp
        src/render/material_resource_residency.cpp
        src/render/main_material_table.cpp
        src/render/shadow_material_table.cpp
        src/render/voxel_reference_renderer.cpp
        src/render/simulation_gpu.cpp)
    if(DVE_ENABLE_CPU_HAIR)
        target_sources(dve_render_bridge PRIVATE src/render/cpu_hair_renderer.cpp)
    endif()
    add_library(dve::render_bridge ALIAS dve_render_bridge)
    target_include_directories(dve_render_bridge PUBLIC include)
    target_compile_features(dve_render_bridge PUBLIC cxx_std_23)
    target_link_libraries(dve_render_bridge PUBLIC dve::core dve::rhi)
    if(MSVC)
        target_compile_options(dve_render_bridge PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_render_bridge PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_render_bridge PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_render_bridge PUBLIC -fsanitize=address,undefined)
    endif()
    add_executable(dve_gabor_volume_v151_demo apps/gabor_volume_v151_demo.cpp)
    target_link_libraries(dve_gabor_volume_v151_demo PRIVATE dve::render_bridge)
    add_executable(dve_material_preview apps/material_preview_v174.cpp)
    target_link_libraries(dve_material_preview PRIVATE dve::render_bridge)
endif()

if(DVE_BUILD_ASSET_COOKER OR DVE_BUILD_BENCHMARK OR DVE_BUILD_EDITOR OR DVE_BUILD_TESTS)
    add_executable(dve_geometry_graph_bake apps/dve_geometry_graph_bake.cpp)
    target_link_libraries(dve_geometry_graph_bake PRIVATE dve::core)
endif()

if(DVE_BUILD_ASSET_COOKER OR DVE_BUILD_BENCHMARK OR DVE_BUILD_EDITOR OR
   (DVE_BUILD_TESTS AND DVE_BUILD_ASSET_PIPELINE_TESTS))
    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)
    add_library(dve_asset_pipeline src/asset_cooker.cpp src/polygon_cooker.cpp src/brick_palette_asset_pipeline.cpp src/heightmap_terrain.cpp)
    add_library(dve::asset_pipeline ALIAS dve_asset_pipeline)
    target_include_directories(dve_asset_pipeline PUBLIC include)
    target_compile_features(dve_asset_pipeline PUBLIC cxx_std_23)
    target_link_libraries(dve_asset_pipeline PUBLIC dve::core PNG::PNG JPEG::JPEG)
    if(MSVC)
        target_compile_options(dve_asset_pipeline PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_asset_pipeline PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_asset_pipeline PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_asset_pipeline PUBLIC -fsanitize=address,undefined)
    endif()

    add_library(dve_sprite_tools
        src/editor_sprite_authoring.cpp
        src/sprite_palette_authoring.cpp
        src/sprite_animation_authoring.cpp
        src/sprite_animation_graph_renderer.cpp
        src/sprite_production_tools.cpp
        src/sprite_pixel_art.cpp)
    add_library(dve::sprite_tools ALIAS dve_sprite_tools)
    target_include_directories(dve_sprite_tools PUBLIC include)
    target_compile_features(dve_sprite_tools PUBLIC cxx_std_23)
    target_link_libraries(dve_sprite_tools PUBLIC dve::core PNG::PNG JPEG::JPEG)
    if(MSVC)
        target_compile_options(dve_sprite_tools PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_sprite_tools PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_sprite_tools PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_sprite_tools PUBLIC -fsanitize=address,undefined)
    endif()

    add_executable(dve_cook_heightmap apps/dve_cook_heightmap.cpp)
    target_link_libraries(dve_cook_heightmap PRIVATE dve::asset_pipeline)

    add_executable(dve_cook_sprite apps/dve_cook_sprite.cpp)
    target_link_libraries(dve_cook_sprite PRIVATE dve::sprite_tools)
    add_executable(dve_sprite_authoring_v222_demo apps/sprite_authoring_v222_demo.cpp)
    target_link_libraries(dve_sprite_authoring_v222_demo PRIVATE dve::sprite_tools)
    target_compile_definitions(dve_sprite_authoring_v222_demo PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(dve_generate_sprite_authoring_assets_v222 apps/generate_sprite_authoring_assets_v222.cpp)
    target_link_libraries(dve_generate_sprite_authoring_assets_v222 PRIVATE dve::sprite_tools)
endif()

if(DVE_BUILD_EDITOR)
    if(NOT DVE_BUILD_AUDIO_SYNTH)
        message(FATAL_ERROR "DVE_BUILD_EDITOR requires DVE_BUILD_AUDIO_SYNTH=ON")
    endif()
    if(NOT DVE_BUILD_PLATFORM)
        message(FATAL_ERROR "DVE_BUILD_EDITOR requires DVE_BUILD_PLATFORM=ON")
    endif()
    add_library(dve_editor
        src/editor_document.cpp
        src/editor_scene_composition.cpp
        src/editor_component_inspector.cpp
        src/editor_control_rig.cpp
        src/editor_prefab.cpp
        src/editor_command.cpp
        src/editor_tools.cpp
        src/editor_workspace.cpp
        src/editor_play_session.cpp
        src/editor_asset_browser.cpp
        src/editor_ai_tools.cpp
        src/editor_settings.cpp
        src/editor_shortcuts.cpp
        src/editor_camera_sequencer.cpp
        src/editor_cinematic_camera.cpp
        src/editor_import_workflow.cpp
        src/editor_import_preview.cpp
        src/editor_project.cpp
        src/editor_file_workflow.cpp
        src/editor_materials.cpp
        src/editor_text3d.cpp
        src/editor_viewport.cpp
        src/editor_diagnostics.cpp
        src/editor_diagnostics_background.cpp
        src/editor_accessibility.cpp
        src/editor_native.cpp
        src/editor_native_renderer.cpp
        src/editor_synth.cpp
        src/editor_chiptune.cpp
        src/editor_audio.cpp
        src/editor_audio_timeline.cpp
        src/editor_audio_workspace.cpp
        src/editor_audio_event.cpp
        src/editor_platform_bridge.cpp)
    add_library(dve::editor ALIAS dve_editor)
    target_include_directories(dve_editor PUBLIC include)
    target_compile_features(dve_editor PUBLIC cxx_std_23)
    target_link_libraries(dve_editor PUBLIC dve::core dve::asset_pipeline dve::sprite_tools dve::platform dve::audio_synth)
    if(TARGET dve_sprite_vertical_slice_presentation)
        target_link_libraries(dve_editor PUBLIC dve::sprite_vertical_slice_presentation)
    endif()
    if(MSVC)
        target_compile_options(dve_editor PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_editor PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_editor PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_editor PUBLIC -fsanitize=address,undefined)
    endif()
    add_executable(dve_editor_demo apps/dve_editor_demo.cpp)
    target_link_libraries(dve_editor_demo PRIVATE dve::editor)
    add_executable(dve_asset_index apps/dve_asset_index.cpp)
    target_link_libraries(dve_asset_index PRIVATE dve::editor)
    add_executable(dve_prefab_tool apps/dve_prefab_tool.cpp)
    target_link_libraries(dve_prefab_tool PRIVATE dve::editor)
    add_executable(dve_camera_settings_v135_demo apps/camera_settings_v135_demo.cpp)
    target_link_libraries(dve_camera_settings_v135_demo PRIVATE dve::editor)
    add_executable(dve_camera_runtime_v136_demo apps/camera_runtime_v136_demo.cpp)
    target_link_libraries(dve_camera_runtime_v136_demo PRIVATE dve::editor)
    add_executable(dve_camera_render_v137_demo apps/camera_render_v137_demo.cpp)
    target_link_libraries(dve_camera_render_v137_demo PRIVATE dve::editor dve::render_bridge)
    add_executable(dve_shortcut_system_v138_demo apps/shortcut_system_v138_demo.cpp)
    target_link_libraries(dve_shortcut_system_v138_demo PRIVATE dve::editor)
    add_executable(dve_ai_assistant_editor_v139_demo apps/ai_assistant_editor_v139_demo.cpp)
    target_link_libraries(dve_ai_assistant_editor_v139_demo PRIVATE dve::editor)
    add_executable(dve_audio_workstation_v130_demo apps/audio_workstation_v130_demo.cpp)
    target_link_libraries(dve_audio_workstation_v130_demo PRIVATE dve::editor)

    if(DVE_BUILD_NATIVE_EDITOR AND TARGET dve_platform_sdl3)
        add_library(dve_editor_sdl_canvas src/editor_sdl_canvas.cpp)
        add_library(dve::editor_sdl_canvas ALIAS dve_editor_sdl_canvas)
        target_include_directories(dve_editor_sdl_canvas PUBLIC include)
        target_compile_features(dve_editor_sdl_canvas PUBLIC cxx_std_23)
        target_link_libraries(dve_editor_sdl_canvas PUBLIC dve::editor dve::platform_sdl3 ${DVE_SDL3_TARGET})
        if(MSVC)
            target_compile_options(dve_editor_sdl_canvas PRIVATE /W4 /permissive-)
        else()
            target_compile_options(dve_editor_sdl_canvas PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        endif()
        add_library(dve_audio_sdl3 src/audio/sdl_synth_audio_device.cpp src/audio/sdl_audio_input_device.cpp)
        add_library(dve::audio_sdl3 ALIAS dve_audio_sdl3)
        target_include_directories(dve_audio_sdl3 PUBLIC include)
        target_compile_features(dve_audio_sdl3 PUBLIC cxx_std_23)
        target_link_libraries(dve_audio_sdl3 PUBLIC dve::audio_synth ${DVE_SDL3_TARGET})
        if(MSVC)
            target_compile_options(dve_audio_sdl3 PRIVATE /W4 /permissive-)
        else()
            target_compile_options(dve_audio_sdl3 PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        endif()
        add_executable(dve_desktop_editor apps/dve_desktop_editor.cpp)
        target_link_libraries(dve_desktop_editor PRIVATE dve::editor_sdl_canvas dve::audio_sdl3)
    endif()

    if(DVE_BUILD_NATIVE_EDITOR AND DVE_BUILD_LEGACY_X11_HOST AND UNIX AND NOT APPLE)
        find_package(X11 QUIET)
        if(X11_FOUND)
            add_executable(dve_native_editor_x11 apps/dve_native_editor_x11.cpp)
            target_include_directories(dve_native_editor_x11 PRIVATE ${X11_INCLUDE_DIR})
            target_link_libraries(dve_native_editor_x11 PRIVATE dve::editor ${X11_LIBRARIES})
            target_compile_definitions(dve_native_editor_x11 PRIVATE DVE_HAVE_X11=1)
        else()
            message(STATUS "X11 development files were not found; native editor executable is disabled")
        endif()
    endif()
endif()

if(DVE_BUILD_TESTS)
    add_executable(dve_v235_foundations_tests tests/test_v235_foundations.cpp)
    target_link_libraries(dve_v235_foundations_tests PRIVATE dve::core)
    add_test(NAME dve_v235_foundations_tests COMMAND dve_v235_foundations_tests)

    enable_testing()
    add_executable(dve_camera_system_tests tests/test_camera_system.cpp)
    target_link_libraries(dve_camera_system_tests PRIVATE dve::core)
    add_test(NAME dve_camera_system_tests COMMAND dve_camera_system_tests)
    add_executable(dve_tilemap2d_tests tests/test_tilemap2d.cpp)
    target_link_libraries(dve_tilemap2d_tests PRIVATE dve::core)
    add_test(NAME dve_tilemap2d_tests COMMAND dve_tilemap2d_tests)
    add_executable(dve_v213_tilemap_authoring_tests tests/test_tilemap_authoring_v213.cpp)
    target_link_libraries(dve_v213_tilemap_authoring_tests PRIVATE dve::core)
    add_test(NAME dve_v213_tilemap_authoring_tests COMMAND dve_v213_tilemap_authoring_tests)
    if(DVE_ENABLE_CPU_HAIR)
        add_executable(dve_v216_mainline_merge_tests tests/test_v216_mainline_merge.cpp)
        target_link_libraries(dve_v216_mainline_merge_tests PRIVATE dve::core)
        add_test(NAME dve_v216_mainline_merge_tests COMMAND dve_v216_mainline_merge_tests)
    endif()
    add_executable(dve_physics2d_tests tests/test_physics2d.cpp)
    target_link_libraries(dve_physics2d_tests PRIVATE dve::core)
    add_test(NAME dve_physics2d_tests COMMAND dve_physics2d_tests)
    add_executable(dve_character_controller2d_tests tests/test_character_controller2d.cpp)
    target_link_libraries(dve_character_controller2d_tests PRIVATE dve::core)
    add_test(NAME dve_character_controller2d_tests COMMAND dve_character_controller2d_tests)
    if(TARGET dve_sprite_tools)
        add_executable(dve_v204_sprite_authoring_tests tests/test_sprite_authoring_v204.cpp)
        target_link_libraries(dve_v204_sprite_authoring_tests PRIVATE dve::sprite_tools)
        add_test(NAME dve_v204_sprite_authoring_tests COMMAND dve_v204_sprite_authoring_tests)
        add_executable(dve_v205_sprite_cook_tests tests/test_sprite_cook_v205.cpp)
        target_link_libraries(dve_v205_sprite_cook_tests PRIVATE dve::sprite_tools)
        add_test(NAME dve_v205_sprite_cook_tests COMMAND dve_v205_sprite_cook_tests)
        add_executable(dve_v206_sprite_palette_cook_tests tests/test_sprite_palette_cook_v206.cpp)
        target_link_libraries(dve_v206_sprite_palette_cook_tests PRIVATE dve::sprite_tools)
        add_test(NAME dve_v206_sprite_palette_cook_tests COMMAND dve_v206_sprite_palette_cook_tests)
        add_executable(dve_v222_sprite_pixel_art_tests tests/test_sprite_pixel_art_v222.cpp)
        target_link_libraries(dve_v222_sprite_pixel_art_tests PRIVATE dve::sprite_tools)
        target_compile_definitions(dve_v222_sprite_pixel_art_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_v222_sprite_pixel_art_tests COMMAND dve_v222_sprite_pixel_art_tests)
        add_executable(dve_v222_sprite_rig2d_tests tests/test_sprite_rig2d_v222.cpp)
        target_link_libraries(dve_v222_sprite_rig2d_tests PRIVATE dve::core)
        add_test(NAME dve_v222_sprite_rig2d_tests COMMAND dve_v222_sprite_rig2d_tests)
        add_executable(dve_v207_sprite_palette_authoring_tests tests/test_sprite_palette_authoring_v207.cpp)
        target_link_libraries(dve_v207_sprite_palette_authoring_tests PRIVATE dve::sprite_tools)
        add_test(NAME dve_v207_sprite_palette_authoring_tests COMMAND dve_v207_sprite_palette_authoring_tests)
        add_executable(dve_v208_sprite_tracks_tests tests/test_sprite_tracks_v208.cpp)
        target_link_libraries(dve_v208_sprite_tracks_tests PRIVATE dve::sprite_tools)
        add_test(NAME dve_v208_sprite_tracks_tests COMMAND dve_v208_sprite_tracks_tests)
        add_executable(dve_v209_sprite_precision_tests tests/test_sprite_precision_v209.cpp)
        target_link_libraries(dve_v209_sprite_precision_tests PRIVATE dve::sprite_tools)

        add_executable(dve_v210_sprite_animation_tests tests/test_sprite_animation_v210.cpp)
        target_link_libraries(dve_v210_sprite_animation_tests PRIVATE dve::sprite_tools)
        add_executable(dve_v211_sprite_animation_tests tests/test_sprite_animation_v211.cpp)
        target_link_libraries(dve_v211_sprite_animation_tests PRIVATE dve::sprite_tools)
        add_executable(dve_v212_sprite_animation_tests tests/test_sprite_animation_v212.cpp)
        target_link_libraries(dve_v212_sprite_animation_tests PRIVATE dve::sprite_tools)
        add_test(NAME dve_v209_sprite_precision_tests COMMAND dve_v209_sprite_precision_tests)
        add_test(NAME dve_v210_sprite_animation_tests COMMAND dve_v210_sprite_animation_tests)
        add_test(NAME dve_v211_sprite_animation_tests COMMAND dve_v211_sprite_animation_tests)
        add_test(NAME dve_v212_sprite_animation_tests COMMAND dve_v212_sprite_animation_tests)
        add_executable(dve_v218_sprite_production_tests tests/test_sprite_production_v218.cpp)
        target_link_libraries(dve_v218_sprite_production_tests PRIVATE dve::sprite_tools)
        target_compile_definitions(dve_v218_sprite_production_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_v218_sprite_production_tests COMMAND dve_v218_sprite_production_tests)
    endif()
    if(DVE_ENABLE_VFX_PARTICLES)
        add_executable(dve_v218_sprite_particle_tests tests/test_sprite_particle_v218.cpp)
        target_link_libraries(dve_v218_sprite_particle_tests PRIVATE dve::core)
        add_test(NAME dve_v218_sprite_particle_tests COMMAND dve_v218_sprite_particle_tests)
    endif()
    if(TARGET dve_sprite_vertical_slice_presentation)
        add_executable(dve_v218_sprite_vertical_slice_presentation_tests
            tests/test_sprite_vertical_slice_presentation_v218.cpp)
        target_link_libraries(dve_v218_sprite_vertical_slice_presentation_tests
            PRIVATE dve::sprite_vertical_slice_presentation)
        target_compile_definitions(dve_v218_sprite_vertical_slice_presentation_tests
            PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_v218_sprite_vertical_slice_presentation_tests
            COMMAND dve_v218_sprite_vertical_slice_presentation_tests)
    endif()
    add_executable(dve_v223_render3d_diagnostics_tests tests/test_render3d_diagnostics_v223.cpp)
    target_link_libraries(dve_v223_render3d_diagnostics_tests PRIVATE dve::core)
    add_test(NAME dve_v223_render3d_diagnostics_tests COMMAND dve_v223_render3d_diagnostics_tests)
    add_executable(dve_v225_brick_palette_tests tests/test_brick_palette_v225.cpp)
    target_link_libraries(dve_v225_brick_palette_tests PRIVATE dve::core)
    add_test(NAME dve_v225_brick_palette_tests COMMAND dve_v225_brick_palette_tests)

    add_executable(dve_v225_brick_palette_shading_tests tests/test_brick_palette_shading_v225.cpp)
    target_link_libraries(dve_v225_brick_palette_shading_tests PRIVATE dve::core)
    add_test(NAME dve_v225_brick_palette_shading_tests COMMAND dve_v225_brick_palette_shading_tests)

    add_executable(dve_v225_voxel_material_switcher_runtime_tests tests/test_voxel_material_switcher_runtime_v225.cpp)
    target_link_libraries(dve_v225_voxel_material_switcher_runtime_tests PRIVATE dve::core)
    add_test(NAME dve_v225_voxel_material_switcher_runtime_tests COMMAND dve_v225_voxel_material_switcher_runtime_tests)

    add_executable(dve_v226_brick_palette_asset_cooker_tests tests/test_brick_palette_asset_cooker_v226.cpp)
    target_link_libraries(dve_v226_brick_palette_asset_cooker_tests PRIVATE dve::core)
    add_test(NAME dve_v226_brick_palette_asset_cooker_tests COMMAND dve_v226_brick_palette_asset_cooker_tests)

    if(TARGET dve_render_bridge)
        add_executable(dve_v226_brick_palette_rhi_tests tests/test_brick_palette_rhi_v226.cpp)
        target_link_libraries(dve_v226_brick_palette_rhi_tests PRIVATE dve::render_bridge)
        add_test(NAME dve_v226_brick_palette_rhi_tests COMMAND dve_v226_brick_palette_rhi_tests)

        add_executable(dve_v227_brick_palette_gpu_resolve_tests
            tests/test_brick_palette_gpu_resolve_v227.cpp)
        target_link_libraries(dve_v227_brick_palette_gpu_resolve_tests PRIVATE dve::render_bridge)
        target_compile_definitions(dve_v227_brick_palette_gpu_resolve_tests PRIVATE
            DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_v227_brick_palette_gpu_resolve_tests
            COMMAND dve_v227_brick_palette_gpu_resolve_tests)
    endif()

    add_executable(dve_v224_voxel_material_policy_tests tests/test_voxel_material_policy_v224.cpp)
    target_link_libraries(dve_v224_voxel_material_policy_tests PRIVATE dve::core)
    add_test(NAME dve_v224_voxel_material_policy_tests COMMAND dve_v224_voxel_material_policy_tests)
    add_executable(dve_v219_sprite_diagnostics_tests tests/test_sprite_diagnostics_v219.cpp)
    target_link_libraries(dve_v219_sprite_diagnostics_tests PRIVATE dve::core)
    add_test(NAME dve_v219_sprite_diagnostics_tests COMMAND dve_v219_sprite_diagnostics_tests)
    add_executable(dve_v219_tile_world_desktop_tests tests/test_tile_world_desktop_v219.cpp)
    target_link_libraries(dve_v219_tile_world_desktop_tests PRIVATE dve::core)
    target_compile_definitions(dve_v219_tile_world_desktop_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_v219_tile_world_desktop_tests COMMAND dve_v219_tile_world_desktop_tests)
    add_executable(dve_v206_sprite_palette_tests tests/test_sprite_palette_v206.cpp)
    target_link_libraries(dve_v206_sprite_palette_tests PRIVATE dve::core)
    add_test(NAME dve_v206_sprite_palette_tests COMMAND dve_v206_sprite_palette_tests)
    if(TARGET dve_render_bridge)
        add_executable(dve_v207_sprite_indexed_rhi_tests tests/test_sprite_indexed_rhi_v207.cpp)
        target_link_libraries(dve_v207_sprite_indexed_rhi_tests PRIVATE dve::render_bridge)
        add_test(NAME dve_v207_sprite_indexed_rhi_tests COMMAND dve_v207_sprite_indexed_rhi_tests)
    endif()
    add_executable(dve_camera_runtime_tests tests/test_camera_runtime.cpp)
    target_link_libraries(dve_camera_runtime_tests PRIVATE dve::core)
    add_test(NAME dve_camera_runtime_tests COMMAND dve_camera_runtime_tests)
    add_executable(dve_v221_display_presentation_tests tests/test_display_presentation_v221.cpp)
    target_link_libraries(dve_v221_display_presentation_tests PRIVATE dve::core)
    add_test(NAME dve_v221_display_presentation_tests COMMAND dve_v221_display_presentation_tests)
    if(DVE_BUILD_RHI)
        add_executable(dve_camera_frame_graph_tests tests/test_camera_frame_graph.cpp)
        target_link_libraries(dve_camera_frame_graph_tests PRIVATE dve::render_bridge dve::rhi)
        add_test(NAME dve_camera_frame_graph_tests COMMAND dve_camera_frame_graph_tests)
        add_executable(dve_camera_cinematic_tests tests/test_camera_cinematic.cpp)
        target_link_libraries(dve_camera_cinematic_tests PRIVATE dve::render_bridge dve::rhi)
        add_test(NAME dve_camera_cinematic_tests COMMAND dve_camera_cinematic_tests)
        add_executable(dve_v2291_camera_optics_tests tests/test_camera_optics_v2291.cpp)
        target_link_libraries(dve_v2291_camera_optics_tests PRIVATE dve::render_bridge dve::rhi)
        add_test(NAME dve_v2291_camera_optics_tests COMMAND dve_v2291_camera_optics_tests)
        add_executable(dve_v221_multi_surface_presenter_tests tests/test_multi_surface_presenter_v221.cpp)
        target_link_libraries(dve_v221_multi_surface_presenter_tests PRIVATE dve::render_bridge dve::rhi)
        add_test(NAME dve_v221_multi_surface_presenter_tests COMMAND dve_v221_multi_surface_presenter_tests)
    endif()
    add_executable(dve_camera_sequence_tests tests/test_camera_sequence.cpp)
    target_link_libraries(dve_camera_sequence_tests PRIVATE dve::core)
    add_test(NAME dve_camera_sequence_tests COMMAND dve_camera_sequence_tests)
    add_test(NAME dve_shader_validation
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate_shaders.sh")
    find_program(DVE_PYTHON3_EXECUTABLE python3)
    if(DVE_PYTHON3_EXECUTABLE)
        add_test(NAME dve_compiled_shader_reflection_self_test
            COMMAND ${DVE_PYTHON3_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_compiled_shader_reflection.py"
                    --self-test)
    endif()
endif()

target_include_directories(dve_core PUBLIC include)
target_compile_features(dve_core PUBLIC cxx_std_23)

if(MSVC)
    target_compile_options(dve_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(dve_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(dve_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(dve_core PUBLIC -fsanitize=address,undefined)
endif()

# Optional Box2D 3.1.x/3.2.x backend for sprite games and general 2D rigid-body simulation.
# Dependency paths, in order: installed package, pinned source checkout, explicit FetchContent,
# then a legacy include/library pair. The native deterministic tile solver remains available in
# every build and is still the default backend.
if(DVE_ENABLE_BOX2D)
    set(DVE_BOX2D_TARGET "")
    set(DVE_BOX2D_VERSION_STRING "")

    find_package(box2d 3.1 CONFIG QUIET)
    if(TARGET box2d::box2d)
        set(DVE_BOX2D_TARGET box2d::box2d)
    elseif(TARGET box2d)
        set(DVE_BOX2D_TARGET box2d)
    endif()
    if(DVE_BOX2D_TARGET AND DEFINED box2d_VERSION)
        set(DVE_BOX2D_VERSION_STRING "${box2d_VERSION}")
    endif()

    if(NOT DVE_BOX2D_TARGET AND DVE_BOX2D_SOURCE_DIR)
        if(NOT EXISTS "${DVE_BOX2D_SOURCE_DIR}/include/box2d/box2d.h" OR
           NOT EXISTS "${DVE_BOX2D_SOURCE_DIR}/src/CMakeLists.txt")
            message(FATAL_ERROR "DVE_BOX2D_SOURCE_DIR must point at a Box2D 3.1+ checkout root")
        endif()
        file(STRINGS "${DVE_BOX2D_SOURCE_DIR}/CMakeLists.txt" DVE_BOX2D_VERSION_LINE
            REGEX "^[ \t]*VERSION[ \t]+[0-9]+\.[0-9]+\.[0-9]+")
        string(REGEX MATCH "[0-9]+\.[0-9]+\.[0-9]+" DVE_BOX2D_VERSION_STRING "${DVE_BOX2D_VERSION_LINE}")
        enable_language(C)
        set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
        set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
        set(BOX2D_PROFILE OFF CACHE BOOL "" FORCE)
        set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
        set(BOX2D_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
        set(BOX2D_AVX2 OFF CACHE BOOL "" FORCE)
        add_subdirectory("${DVE_BOX2D_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/box2d-build" EXCLUDE_FROM_ALL)
        if(TARGET box2d::box2d)
            set(DVE_BOX2D_TARGET box2d::box2d)
        elseif(TARGET box2d)
            set(DVE_BOX2D_TARGET box2d)
        endif()
    endif()

    if(NOT DVE_BOX2D_TARGET AND DVE_FETCH_BOX2D)
        include(FetchContent)
        enable_language(C)
        set(BOX2D_SAMPLES OFF CACHE BOOL "" FORCE)
        set(BOX2D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(BOX2D_DOCS OFF CACHE BOOL "" FORCE)
        set(BOX2D_PROFILE OFF CACHE BOOL "" FORCE)
        set(BOX2D_VALIDATE OFF CACHE BOOL "" FORCE)
        set(BOX2D_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
        set(BOX2D_AVX2 OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(Box2D
            GIT_REPOSITORY https://github.com/erincatto/box2d.git
            GIT_TAG v3.1.1
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(Box2D)
        set(DVE_BOX2D_VERSION_STRING "3.1.1")
        if(TARGET box2d::box2d)
            set(DVE_BOX2D_TARGET box2d::box2d)
        elseif(TARGET box2d)
            set(DVE_BOX2D_TARGET box2d)
        endif()
    endif()

    if(NOT DVE_BOX2D_TARGET AND BOX2D_INCLUDE_DIR AND BOX2D_LIBRARY)
        add_library(dve_box2d_legacy UNKNOWN IMPORTED)
        set_target_properties(dve_box2d_legacy PROPERTIES
            IMPORTED_LOCATION "${BOX2D_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${BOX2D_INCLUDE_DIR}")
        set(DVE_BOX2D_TARGET dve_box2d_legacy)
        set(DVE_BOX2D_VERSION_STRING "${DVE_BOX2D_LEGACY_VERSION}")
    endif()

    if(NOT DVE_BOX2D_TARGET)
        message(FATAL_ERROR
            "DVE_ENABLE_BOX2D requires an installed Box2D 3.1/3.2 package, DVE_BOX2D_SOURCE_DIR, "
            "DVE_FETCH_BOX2D=ON, or the legacy BOX2D_INCLUDE_DIR/BOX2D_LIBRARY pair")
    endif()
    if(NOT DVE_BOX2D_VERSION_STRING OR DVE_BOX2D_VERSION_STRING VERSION_LESS "3.1.0" OR
       NOT DVE_BOX2D_VERSION_STRING VERSION_LESS "3.3.0")
        message(FATAL_ERROR "DVE supports Box2D 3.1.x and 3.2.x; found '${DVE_BOX2D_VERSION_STRING}'")
    endif()

    target_sources(dve_core PRIVATE src/physics_box2d_backend.cpp)
    target_link_libraries(dve_core PUBLIC ${DVE_BOX2D_TARGET})
    target_compile_definitions(dve_core PUBLIC DVE_HAVE_BOX2D=1)
    if(NOT DVE_BOX2D_VERSION_STRING VERSION_LESS "3.2.0")
        target_compile_definitions(dve_core PRIVATE DVE_BOX2D_API_32=1)
    endif()
    message(STATUS "Box2D ${DVE_BOX2D_VERSION_STRING} sprite/2D physics backend enabled")
    if(DVE_BUILD_TESTS)
        add_executable(dve_box2d_backend_smoke tests/test_physics2d_box2d_smoke.cpp)
        target_link_libraries(dve_box2d_backend_smoke PRIVATE dve::core)
        add_test(NAME dve_box2d_backend_smoke COMMAND dve_box2d_backend_smoke)
        add_executable(dve_box2d_upstream_tests tests/test_physics2d_box2d_upstream.cpp)
        target_link_libraries(dve_box2d_upstream_tests PRIVATE dve::core)
        add_test(NAME dve_box2d_upstream_tests COMMAND dve_box2d_upstream_tests)
    endif()
endif()

# The contract shim exercises the complete DVE adapter without requiring a system Box2D install.
# It intentionally mirrors the public Box2D 3.1.x calls used by the adapter; it is not a
# replacement solver and is never linked into shipping engine targets.
if(DVE_BUILD_TESTS AND DVE_BUILD_BOX2D_CONTRACT_TESTS)
    add_library(dve_box2d_contract_backend STATIC
        src/physics2d.cpp
        src/physics_box2d_backend.cpp
        src/tilemap2d.cpp
        tests/box2d_contract/box2d_contract_stub.cpp)
    target_include_directories(dve_box2d_contract_backend
        PRIVATE tests/box2d_contract/include
        PUBLIC include)
    target_compile_features(dve_box2d_contract_backend PUBLIC cxx_std_23)
    target_compile_definitions(dve_box2d_contract_backend PUBLIC DVE_HAVE_BOX2D=1)
    target_link_libraries(dve_box2d_contract_backend PUBLIC Threads::Threads)
    if(MSVC)
        target_compile_options(dve_box2d_contract_backend PRIVATE /W4 /permissive-)
    else()
        target_compile_options(dve_box2d_contract_backend PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    if(DVE_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(dve_box2d_contract_backend PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(dve_box2d_contract_backend PUBLIC -fsanitize=address,undefined)
    endif()
    add_executable(dve_box2d_contract_tests tests/test_physics2d_box2d_contract.cpp)
    target_link_libraries(dve_box2d_contract_tests PRIVATE dve_box2d_contract_backend)
    add_test(NAME dve_box2d_contract_tests COMMAND dve_box2d_contract_tests)
endif()


if(WIN32 AND DVE_BUILD_RHI)
    # Platform graphics code is no longer compiled into dve_core. Direct3D 12 is an explicit
    # backend target that can be replaced by Vulkan or Metal without contaminating the core.
    add_library(dve_rhi_d3d12
        src/gpu/d3d12_brickmap_backend.cpp
        src/gpu/d3d12_runtime_brickmap_world.cpp)
    add_library(dve::rhi_d3d12 ALIAS dve_rhi_d3d12)
    target_include_directories(dve_rhi_d3d12 PUBLIC include)
    target_compile_features(dve_rhi_d3d12 PUBLIC cxx_std_23)
    target_link_libraries(dve_rhi_d3d12 PUBLIC dve::core dve::rhi d3d12 dxgi)
    if(MSVC)
        target_compile_options(dve_rhi_d3d12 PRIVATE /W4 /permissive-)
    endif()
    add_executable(dve_d3d12_readback_smoke apps/d3d12_readback_smoke.cpp)
    target_link_libraries(dve_d3d12_readback_smoke PRIVATE dve::rhi_d3d12)
endif()

# Optional Box3D 0.1.x backend for three-dimensional rigid bodies, voxel fragments,
# ragdolls, polygon collision proxies, and general game physics. Jolt remains available and
# the engine-facing IRigidBodyWorld contract is shared by both production solvers.
if(DVE_ENABLE_BOX3D)
    set(DVE_BOX3D_TARGET "")
    set(DVE_BOX3D_VERSION_STRING "")

    find_package(box3d 0.1 CONFIG QUIET)
    if(TARGET box3d::box3d)
        set(DVE_BOX3D_TARGET box3d::box3d)
    elseif(TARGET box3d)
        set(DVE_BOX3D_TARGET box3d)
    endif()
    if(DVE_BOX3D_TARGET AND DEFINED box3d_VERSION)
        set(DVE_BOX3D_VERSION_STRING "${box3d_VERSION}")
    endif()

    if(NOT DVE_BOX3D_TARGET AND DVE_BOX3D_SOURCE_DIR)
        if(NOT EXISTS "${DVE_BOX3D_SOURCE_DIR}/include/box3d/box3d.h" OR
           NOT EXISTS "${DVE_BOX3D_SOURCE_DIR}/src/CMakeLists.txt")
            message(FATAL_ERROR "DVE_BOX3D_SOURCE_DIR must point at a Box3D 0.1.x checkout root")
        endif()
        file(STRINGS "${DVE_BOX3D_SOURCE_DIR}/CMakeLists.txt" DVE_BOX3D_VERSION_LINE
            REGEX "^[ \t]*VERSION[ \t]+[0-9]+\.[0-9]+\.[0-9]+")
        string(REGEX MATCH "[0-9]+\.[0-9]+\.[0-9]+" DVE_BOX3D_VERSION_STRING "${DVE_BOX3D_VERSION_LINE}")
        enable_language(C)
        set(BOX3D_SAMPLES OFF CACHE BOOL "" FORCE)
        set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(BOX3D_DOCS OFF CACHE BOOL "" FORCE)
        set(BOX3D_PROFILE OFF CACHE BOOL "" FORCE)
        set(BOX3D_VALIDATE OFF CACHE BOOL "" FORCE)
        set(BOX3D_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
        add_subdirectory("${DVE_BOX3D_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/box3d-build" EXCLUDE_FROM_ALL)
        if(TARGET box3d::box3d)
            set(DVE_BOX3D_TARGET box3d::box3d)
        elseif(TARGET box3d)
            set(DVE_BOX3D_TARGET box3d)
        endif()
    endif()

    if(NOT DVE_BOX3D_TARGET AND DVE_FETCH_BOX3D)
        include(FetchContent)
        enable_language(C)
        set(BOX3D_SAMPLES OFF CACHE BOOL "" FORCE)
        set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
        set(BOX3D_DOCS OFF CACHE BOOL "" FORCE)
        set(BOX3D_PROFILE OFF CACHE BOOL "" FORCE)
        set(BOX3D_VALIDATE OFF CACHE BOOL "" FORCE)
        set(BOX3D_COMPILE_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            box3d
            GIT_REPOSITORY https://github.com/erincatto/box3d.git
            GIT_TAG 23861418d877fdd97990f35645e845e44a7eb9ec
            GIT_SHALLOW FALSE)
        FetchContent_MakeAvailable(box3d)
        set(DVE_BOX3D_VERSION_STRING "0.1.0")
        if(TARGET box3d::box3d)
            set(DVE_BOX3D_TARGET box3d::box3d)
        elseif(TARGET box3d)
            set(DVE_BOX3D_TARGET box3d)
        endif()
    endif()

    if(NOT DVE_BOX3D_TARGET AND BOX3D_INCLUDE_DIR AND BOX3D_LIBRARY)
        add_library(dve_box3d_legacy UNKNOWN IMPORTED)
        set_target_properties(dve_box3d_legacy PROPERTIES
            IMPORTED_LOCATION "${BOX3D_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${BOX3D_INCLUDE_DIR}")
        set(DVE_BOX3D_TARGET dve_box3d_legacy)
        set(DVE_BOX3D_VERSION_STRING "${DVE_BOX3D_LEGACY_VERSION}")
    endif()

    if(NOT DVE_BOX3D_TARGET)
        message(FATAL_ERROR
            "DVE_ENABLE_BOX3D requires an installed Box3D 0.1 package, DVE_BOX3D_SOURCE_DIR, "
            "DVE_FETCH_BOX3D=ON, or the legacy BOX3D_INCLUDE_DIR/BOX3D_LIBRARY pair")
    endif()
    if(NOT DVE_BOX3D_VERSION_STRING OR DVE_BOX3D_VERSION_STRING VERSION_LESS "0.1.0" OR
       NOT DVE_BOX3D_VERSION_STRING VERSION_LESS "0.2.0")
        message(FATAL_ERROR "DVE supports Box3D 0.1.x; found '${DVE_BOX3D_VERSION_STRING}'")
    endif()

    target_sources(dve_core PRIVATE src/physics_box3d_backend.cpp)
    target_link_libraries(dve_core PUBLIC ${DVE_BOX3D_TARGET})
    target_compile_definitions(dve_core PUBLIC DVE_HAVE_BOX3D=1)
    message(STATUS "Box3D ${DVE_BOX3D_VERSION_STRING} 3D physics backend enabled")

    if(DVE_BUILD_TESTS)
        add_executable(dve_box3d_upstream_tests tests/test_physics_box3d_upstream.cpp)
        target_link_libraries(dve_box3d_upstream_tests PRIVATE dve::core)
        add_test(NAME dve_box3d_upstream_tests COMMAND dve_box3d_upstream_tests)
    endif()
endif()

# Jolt Physics backend (WP-4.4). The preferred dependency paths are, in order:
#   1. an installed Jolt::Jolt package,
#   2. a pinned source checkout passed with DVE_JOLT_SOURCE_DIR,
#   3. optional FetchContent of the immutable v5.6.0 release tag,
#   4. the legacy include/library pair for existing build farms.
#
# Jolt's compile-time feature macros must match between the library and this target. The
# source/FetchContent paths below set the exact low-dependency configuration validated for the
# engine. The legacy path remains available but is deliberately labeled as less reproducible.
if(DVE_ENABLE_JOLT)
    set(DVE_JOLT_TARGET "")

    set(DVE_JOLT_VERSION_STRING "")
    find_package(Jolt 5.5 CONFIG QUIET)
    if(TARGET Jolt::Jolt)
        set(DVE_JOLT_TARGET Jolt::Jolt)
    elseif(TARGET Jolt)
        set(DVE_JOLT_TARGET Jolt)
    endif()
    if(DVE_JOLT_TARGET)
        if(DEFINED Jolt_VERSION)
            set(DVE_JOLT_VERSION_STRING "${Jolt_VERSION}")
        elseif(DEFINED JOLT_VERSION)
            set(DVE_JOLT_VERSION_STRING "${JOLT_VERSION}")
        endif()
        if(DVE_JOLT_VERSION_STRING AND NOT DVE_JOLT_VERSION_STRING MATCHES "^5[.][56][.][0-9]+([.-].*)?$")
            message(FATAL_ERROR "DVE supports installed JoltPhysics 5.5.x-5.6.x; found ${DVE_JOLT_VERSION_STRING}")
        endif()
    endif()

    if(NOT DVE_JOLT_TARGET AND DVE_JOLT_SOURCE_DIR)
        if(NOT EXISTS "${DVE_JOLT_SOURCE_DIR}/Jolt/Jolt.h" OR
           NOT EXISTS "${DVE_JOLT_SOURCE_DIR}/Build/CMakeLists.txt")
            message(FATAL_ERROR "DVE_JOLT_SOURCE_DIR must point at a JoltPhysics 5.5.x-5.6.x checkout root")
        endif()
        file(STRINGS "${DVE_JOLT_SOURCE_DIR}/Jolt/Core/Core.h" DVE_JOLT_MAJOR_LINE
            REGEX "^#define JPH_VERSION_MAJOR ")
        file(STRINGS "${DVE_JOLT_SOURCE_DIR}/Jolt/Core/Core.h" DVE_JOLT_MINOR_LINE
            REGEX "^#define JPH_VERSION_MINOR ")
        file(STRINGS "${DVE_JOLT_SOURCE_DIR}/Jolt/Core/Core.h" DVE_JOLT_PATCH_LINE
            REGEX "^#define JPH_VERSION_PATCH ")
        string(REGEX REPLACE ".*JPH_VERSION_MAJOR[ \t]+([0-9]+).*" "\\1" DVE_JOLT_MAJOR "${DVE_JOLT_MAJOR_LINE}")
        string(REGEX REPLACE ".*JPH_VERSION_MINOR[ \t]+([0-9]+).*" "\\1" DVE_JOLT_MINOR "${DVE_JOLT_MINOR_LINE}")
        string(REGEX REPLACE ".*JPH_VERSION_PATCH[ \t]+([0-9]+).*" "\\1" DVE_JOLT_PATCH "${DVE_JOLT_PATCH_LINE}")
        set(DVE_JOLT_VERSION_STRING "${DVE_JOLT_MAJOR}.${DVE_JOLT_MINOR}.${DVE_JOLT_PATCH}")
        if(NOT DVE_JOLT_VERSION_STRING MATCHES "^5[.][56][.][0-9]+$")
            message(FATAL_ERROR "DVE supports JoltPhysics 5.5.x-5.6.x; found ${DVE_JOLT_VERSION_STRING}")
        endif()
        set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
        set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
        # UBSan's vptr instrumentation requires matching C++ RTTI across the DVE/Jolt boundary.
        # Production builds retain Jolt's lower-overhead no-RTTI default.
        if(DVE_ENABLE_SANITIZERS OR DVE_ENABLE_JOLT_DEBUG_DRAW)
            set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
        else()
            set(CPP_RTTI_ENABLED OFF CACHE BOOL "" FORCE)
        endif()
        set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ${DVE_ENABLE_JOLT_DEBUG_DRAW} CACHE BOOL "" FORCE)
        set(DEBUG_RENDERER_IN_DISTRIBUTION ${DVE_ENABLE_JOLT_DEBUG_DRAW} CACHE BOOL "" FORCE)
        set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
        set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
        set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
        set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
        set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
        set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
        add_subdirectory("${DVE_JOLT_SOURCE_DIR}/Build" "${CMAKE_BINARY_DIR}/_deps/jolt-build" EXCLUDE_FROM_ALL)
        set(DVE_JOLT_TARGET Jolt)
    endif()

    if(NOT DVE_JOLT_TARGET AND DVE_FETCH_JOLT)
        include(FetchContent)
        set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
        set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
        # UBSan's vptr instrumentation requires matching C++ RTTI across the DVE/Jolt boundary.
        # Production builds retain Jolt's lower-overhead no-RTTI default.
        if(DVE_ENABLE_SANITIZERS OR DVE_ENABLE_JOLT_DEBUG_DRAW)
            set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
        else()
            set(CPP_RTTI_ENABLED OFF CACHE BOOL "" FORCE)
        endif()
        set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE ${DVE_ENABLE_JOLT_DEBUG_DRAW} CACHE BOOL "" FORCE)
        set(DEBUG_RENDERER_IN_DISTRIBUTION ${DVE_ENABLE_JOLT_DEBUG_DRAW} CACHE BOOL "" FORCE)
        set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
        set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
        set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
        set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
        set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
        set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
            JoltPhysics
            GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
            GIT_TAG v5.6.0
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR Build)
        FetchContent_MakeAvailable(JoltPhysics)
        set(DVE_JOLT_TARGET Jolt)
    endif()

    if(NOT DVE_JOLT_TARGET AND JOLT_INCLUDE_DIR AND JOLT_LIBRARY)
        if(NOT DVE_JOLT_LEGACY_VERSION MATCHES "^5[.][56][.][0-9]+([.-].*)?$")
            message(FATAL_ERROR "Legacy JOLT_INCLUDE_DIR/JOLT_LIBRARY requires DVE_JOLT_LEGACY_VERSION in the supported 5.5.x-5.6.x range")
        endif()
        set(DVE_JOLT_VERSION_STRING "${DVE_JOLT_LEGACY_VERSION}")
        message(WARNING "Using legacy JOLT_INCLUDE_DIR/JOLT_LIBRARY for Jolt ${DVE_JOLT_VERSION_STRING}; matching feature flags remain the caller's responsibility")
        add_library(dve_jolt_legacy UNKNOWN IMPORTED)
        set_target_properties(dve_jolt_legacy PROPERTIES
            IMPORTED_LOCATION "${JOLT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${JOLT_INCLUDE_DIR}")
        set(DVE_JOLT_TARGET dve_jolt_legacy)
    endif()

    if(NOT DVE_JOLT_TARGET)
        message(FATAL_ERROR
            "DVE_ENABLE_JOLT requires an installed Jolt 5.5/5.6 package, DVE_JOLT_SOURCE_DIR, "
            "DVE_FETCH_JOLT=ON, or the legacy JOLT_INCLUDE_DIR/JOLT_LIBRARY pair")
    endif()

    if(NOT DVE_JOLT_VERSION_STRING)
        set(DVE_JOLT_VERSION_STRING "5.5.x-5.6.x (compile-time verified)")
    endif()
    message(STATUS "Jolt Physics ${DVE_JOLT_VERSION_STRING} 3D backend enabled")

    target_sources(dve_core PRIVATE src/physics_jolt_backend.cpp src/jolt_replay.cpp)
    target_link_libraries(dve_core PUBLIC ${DVE_JOLT_TARGET})
    target_compile_definitions(dve_core PUBLIC DVE_HAVE_JOLT=1)
    if(NOT MSVC)
        target_compile_options(dve_core PUBLIC -Wno-unused-parameter)
    endif()
    if(DVE_BUILD_JOLT_SMOKE)
        add_executable(dve_jolt_solver_smoke apps/jolt_solver_smoke.cpp)
        target_link_libraries(dve_jolt_solver_smoke PRIVATE dve::core)
        add_executable(dve_jolt_scene_publication_smoke apps/jolt_scene_publication_smoke.cpp)
        target_link_libraries(dve_jolt_scene_publication_smoke PRIVATE dve::core)
        target_compile_definitions(dve_jolt_scene_publication_smoke PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_executable(dve_game_world_jolt_smoke apps/game_world_jolt_smoke.cpp)
        target_link_libraries(dve_game_world_jolt_smoke PRIVATE dve::core)
        if(DVE_BUILD_TESTS)
            add_test(NAME dve_jolt_solver_smoke COMMAND dve_jolt_solver_smoke)
            add_test(NAME dve_jolt_scene_publication_smoke COMMAND dve_jolt_scene_publication_smoke)
            add_test(NAME dve_game_world_jolt_smoke COMMAND dve_game_world_jolt_smoke)
            add_executable(dve_jolt_upstream_tests tests/test_physics_jolt_upstream.cpp)
            target_link_libraries(dve_jolt_upstream_tests PRIVATE dve::core)
            if(NOT MSVC)
                target_compile_options(dve_jolt_upstream_tests PRIVATE -Wall -Wextra -Wconversion -Werror -Wno-unused-parameter)
            endif()
            add_test(NAME dve_jolt_upstream_tests COMMAND dve_jolt_upstream_tests)
            add_executable(dve_jolt_extended_tests tests/test_physics_jolt_extended.cpp)
            target_link_libraries(dve_jolt_extended_tests PRIVATE dve::core)
            if(NOT MSVC)
                target_compile_options(dve_jolt_extended_tests PRIVATE -Wall -Wextra -Wconversion -Werror -Wno-unused-parameter)
            endif()
            add_test(NAME dve_jolt_extended_tests COMMAND dve_jolt_extended_tests)
        endif()
    endif()
    if(DVE_BUILD_BENCHMARK)
        add_executable(dve_jolt_collapse_bench apps/jolt_collapse_bench.cpp)
        target_link_libraries(dve_jolt_collapse_bench PRIVATE dve::core)
    endif()
    if(DVE_BUILD_EDITOR)
        # Simulate mode (v1.14 item 9): only meaningful, and only compiled, when both the
        # editor and a real Jolt backend are present.
        target_sources(dve_editor PRIVATE src/editor_simulation.cpp)
        target_compile_definitions(dve_editor PUBLIC DVE_HAVE_JOLT=1)
        if(DVE_BUILD_TESTS)
            add_executable(dve_editor_jolt_simulation_smoke apps/editor_jolt_simulation_smoke.cpp)
            target_link_libraries(dve_editor_jolt_simulation_smoke PRIVATE dve::editor)
            add_test(NAME dve_editor_jolt_simulation_smoke COMMAND dve_editor_jolt_simulation_smoke)
        endif()
    endif()
endif()

# Optional Lua gameplay scripting. GameWorld is always available in dve_core; Lua only adds
# the script-host binding. Prefer an installed development package, but support Linux systems
# that ship a Lua 5.4 runtime without headers through the narrow bundled ABI declarations.
if(DVE_ENABLE_LUA)
    set(DVE_LUA_TARGET "")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(DVE_LUA QUIET IMPORTED_TARGET lua5.4)
        if(TARGET PkgConfig::DVE_LUA)
            set(DVE_LUA_TARGET PkgConfig::DVE_LUA)
        endif()
    endif()

    if(NOT DVE_LUA_TARGET)
        find_path(DVE_LUA_INCLUDE_DIR lua5.4/lua.h)
        find_library(DVE_LUA_LIBRARY NAMES lua5.4 lua54 lua)
        if(DVE_LUA_INCLUDE_DIR AND DVE_LUA_LIBRARY)
            add_library(dve_lua54_imported UNKNOWN IMPORTED)
            set_target_properties(dve_lua54_imported PROPERTIES
                IMPORTED_LOCATION "${DVE_LUA_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${DVE_LUA_INCLUDE_DIR}")
            set(DVE_LUA_TARGET dve_lua54_imported)
        endif()
    endif()

    if(NOT DVE_LUA_TARGET AND UNIX)
        find_file(DVE_LUA_RUNTIME_LIBRARY
            NAMES liblua5.4.so.0 liblua5.4.dylib
            PATHS /lib /usr/lib /usr/local/lib
            PATH_SUFFIXES x86_64-linux-gnu aarch64-linux-gnu)
        if(DVE_LUA_RUNTIME_LIBRARY)
            add_library(dve_lua54_runtime UNKNOWN IMPORTED)
            set_target_properties(dve_lua54_runtime PROPERTIES
                IMPORTED_LOCATION "${DVE_LUA_RUNTIME_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua54_abi/include")
            set(DVE_LUA_TARGET dve_lua54_runtime)
            message(STATUS "Lua 5.4 runtime found without development headers; using ABI fallback declarations")
        endif()
    endif()

    if(NOT DVE_LUA_TARGET)
        message(FATAL_ERROR "DVE_ENABLE_LUA requires a Lua 5.4 development package or runtime library")
    endif()

    target_sources(dve_core PRIVATE src/game_script.cpp)
    target_link_libraries(dve_core PUBLIC ${DVE_LUA_TARGET})
    target_compile_definitions(dve_core PUBLIC DVE_HAVE_LUA=1)

    if(DVE_BUILD_TESTS)
        add_executable(dve_game_script_tests tests/test_game_script.cpp)
        target_link_libraries(dve_game_script_tests PRIVATE dve::core)
        add_test(NAME dve_game_script_tests COMMAND dve_game_script_tests)
    endif()

    if(DVE_ENABLE_JOLT AND DVE_BUILD_BENCHMARK)
        add_executable(dve_game_script_jolt_smoke apps/game_script_jolt_smoke.cpp)
        target_link_libraries(dve_game_script_jolt_smoke PRIVATE dve::core)
        if(DVE_BUILD_TESTS)
            add_test(NAME dve_game_script_jolt_smoke COMMAND dve_game_script_jolt_smoke)
        endif()
    endif()

    if(DVE_ENABLE_JOLT AND UNIX AND NOT APPLE)
        find_package(X11 QUIET)
        if(X11_FOUND AND X11_XTest_FOUND)
            add_executable(dve_game_host_x11 apps/game_host_x11.cpp)
            target_include_directories(dve_game_host_x11 PRIVATE ${X11_INCLUDE_DIR})
            target_link_libraries(dve_game_host_x11 PRIVATE dve::core ${X11_LIBRARIES} ${X11_XTest_LIB})
            if(DVE_BUILD_TESTS)
                find_program(DVE_GAME_XVFB_RUN xvfb-run)
                if(DVE_GAME_XVFB_RUN)
                    add_test(NAME dve_game_host_x11_smoke
                        COMMAND ${DVE_GAME_XVFB_RUN} -a $<TARGET_FILE:dve_game_host_x11> --smoke)
                endif()
            endif()
        endif()
    endif()
endif()

if(DVE_BUILD_ASSET_COOKER)
    add_executable(dve_cook_model apps/dve_cook_model.cpp)
    target_link_libraries(dve_cook_model PRIVATE dve::asset_pipeline)
    add_executable(dve_cook_mesh apps/dve_cook_mesh.cpp)
    target_link_libraries(dve_cook_mesh PRIVATE dve::asset_pipeline)
endif()

if(DVE_BUILD_BENCHMARK AND DVE_BUILD_RHI)
    add_executable(dve_hybrid_geometry_demo apps/hybrid_geometry_demo.cpp)
    target_link_libraries(dve_hybrid_geometry_demo PRIVATE dve::core dve::render_bridge dve::rhi)
    add_executable(dve_polygon_render_demo apps/polygon_render_demo.cpp)
    target_link_libraries(dve_polygon_render_demo PRIVATE dve::core dve::render_bridge dve::rhi)
endif()

if(DVE_BUILD_TESTS)
    if(DVE_BUILD_PLATFORM AND DVE_BUILD_SDL3_CONTRACT_TESTS)
        add_library(dve_fake_sdl3 STATIC tests/fake_sdl3/fake_sdl3.cpp)
        target_include_directories(dve_fake_sdl3 PUBLIC tests/fake_sdl3)
        target_compile_features(dve_fake_sdl3 PUBLIC cxx_std_23)

        add_library(dve_platform_sdl3_contract STATIC src/platform/sdl_application_host.cpp)
        target_include_directories(dve_platform_sdl3_contract PUBLIC include PRIVATE tests/fake_sdl3)
        target_compile_features(dve_platform_sdl3_contract PUBLIC cxx_std_23)
        target_link_libraries(dve_platform_sdl3_contract PUBLIC dve::platform dve_fake_sdl3)
        if(MSVC)
            target_compile_options(dve_platform_sdl3_contract PRIVATE /W4 /permissive-)
        else()
            target_compile_options(dve_platform_sdl3_contract PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        endif()

        add_executable(dve_sdl_application_host_tests tests/test_sdl_application_host.cpp)
        target_include_directories(dve_sdl_application_host_tests PRIVATE tests/fake_sdl3)
        target_link_libraries(dve_sdl_application_host_tests PRIVATE dve_platform_sdl3_contract)
        add_test(NAME dve_sdl_application_host_tests COMMAND dve_sdl_application_host_tests)

        if(DVE_BUILD_EDITOR)
            add_library(dve_editor_sdl_canvas_contract STATIC src/editor_sdl_canvas.cpp)
            target_include_directories(dve_editor_sdl_canvas_contract PUBLIC include PRIVATE tests/fake_sdl3)
            target_compile_features(dve_editor_sdl_canvas_contract PUBLIC cxx_std_23)
            target_link_libraries(dve_editor_sdl_canvas_contract PUBLIC dve::editor dve_platform_sdl3_contract dve_fake_sdl3)
            if(MSVC)
                target_compile_options(dve_editor_sdl_canvas_contract PRIVATE /W4 /permissive-)
            else()
                target_compile_options(dve_editor_sdl_canvas_contract PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
            endif()
            add_library(dve_audio_sdl3_contract STATIC src/audio/sdl_synth_audio_device.cpp src/audio/sdl_audio_input_device.cpp)
            target_include_directories(dve_audio_sdl3_contract PUBLIC include PRIVATE tests/fake_sdl3)
            target_compile_features(dve_audio_sdl3_contract PUBLIC cxx_std_23)
            target_link_libraries(dve_audio_sdl3_contract PUBLIC dve::audio_synth dve_fake_sdl3)
            if(MSVC)
                target_compile_options(dve_audio_sdl3_contract PRIVATE /W4 /permissive-)
            else()
                target_compile_options(dve_audio_sdl3_contract PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
            endif()
            add_executable(dve_sdl_synth_audio_tests tests/test_sdl_synth_audio.cpp)
            target_include_directories(dve_sdl_synth_audio_tests PRIVATE tests/fake_sdl3)
            target_link_libraries(dve_sdl_synth_audio_tests PRIVATE dve_audio_sdl3_contract dve_fake_sdl3)
            add_test(NAME dve_sdl_synth_audio_tests COMMAND dve_sdl_synth_audio_tests)

            add_executable(dve_desktop_editor_contract apps/dve_desktop_editor.cpp)
            target_include_directories(dve_desktop_editor_contract PRIVATE tests/fake_sdl3)
            target_link_libraries(dve_desktop_editor_contract PRIVATE dve_editor_sdl_canvas_contract dve_audio_sdl3_contract)
            add_test(NAME dve_desktop_editor_contract
                COMMAND dve_desktop_editor_contract --smoke
                    --accessibility-dump ${CMAKE_BINARY_DIR}/desktop_editor_accessibility.json)
        endif()
    endif()

    if(DVE_BUILD_PLATFORM AND DVE_BUILD_RHI)
        add_executable(dve_platform_rhi_tests tests/test_platform_rhi.cpp)
        target_link_libraries(dve_platform_rhi_tests PRIVATE dve::platform dve::rhi dve::render_bridge)
        add_test(NAME dve_platform_rhi_tests COMMAND dve_platform_rhi_tests)
    endif()

    if(TARGET dve_rhi_vulkan)
        add_executable(dve_vulkan_buffer_tests tests/test_vulkan_buffer_device.cpp)
        target_link_libraries(dve_vulkan_buffer_tests PRIVATE dve::rhi_vulkan dve::render_bridge)
        add_test(NAME dve_vulkan_buffer_tests COMMAND dve_vulkan_buffer_tests)
        add_executable(dve_vulkan_graphics_tests tests/test_vulkan_graphics_device.cpp)
        target_link_libraries(dve_vulkan_graphics_tests PRIVATE dve::rhi_vulkan)
        add_test(NAME dve_vulkan_graphics_tests COMMAND dve_vulkan_graphics_tests)
        add_executable(dve_v193_vulkan_sprite_tests tests/test_vulkan_sprite_rhi_v193.cpp)
        target_link_libraries(dve_v193_vulkan_sprite_tests PRIVATE
            dve::rhi_vulkan dve::render_bridge)
        target_compile_definitions(dve_v193_vulkan_sprite_tests PRIVATE
            DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_v193_vulkan_sprite_tests COMMAND dve_v193_vulkan_sprite_tests)
        add_executable(dve_depth_only_rhi_tests tests/test_depth_only_rhi.cpp)
        target_link_libraries(dve_depth_only_rhi_tests PRIVATE dve::rhi_vulkan)
        add_test(NAME dve_depth_only_rhi_tests COMMAND dve_depth_only_rhi_tests)
        add_executable(dve_vulkan_compute_tests tests/test_vulkan_compute_device.cpp)
        target_link_libraries(dve_vulkan_compute_tests PRIVATE dve::rhi_vulkan)
        add_test(NAME dve_vulkan_compute_tests COMMAND dve_vulkan_compute_tests)
        add_executable(dve_vulkan_camera_outputs_tests tests/test_vulkan_camera_outputs.cpp)
        target_link_libraries(dve_vulkan_camera_outputs_tests PRIVATE dve::render_bridge dve::rhi_vulkan)
        add_test(NAME dve_vulkan_camera_outputs_tests COMMAND dve_vulkan_camera_outputs_tests)

        add_executable(dve_vulkan_environment_lighting_tests tests/test_vulkan_environment_lighting.cpp)
        target_link_libraries(dve_vulkan_environment_lighting_tests PRIVATE dve::rhi_vulkan dve::render_bridge)
        add_test(NAME dve_vulkan_environment_lighting_tests COMMAND dve_vulkan_environment_lighting_tests)
        add_executable(dve_vulkan_live_environment_renderer_tests tests/test_vulkan_live_environment_renderer.cpp)
        target_link_libraries(dve_vulkan_live_environment_renderer_tests PRIVATE dve::rhi_vulkan dve::render_bridge)
        add_test(NAME dve_vulkan_live_environment_renderer_tests COMMAND dve_vulkan_live_environment_renderer_tests)
        add_executable(dve_vulkan_shadow_material_table_tests tests/test_vulkan_shadow_material_table.cpp)
        target_link_libraries(dve_vulkan_shadow_material_table_tests PRIVATE dve::rhi_vulkan dve::render_bridge)
        add_test(NAME dve_vulkan_shadow_material_table_tests COMMAND dve_vulkan_shadow_material_table_tests)
        add_executable(dve_vulkan_main_material_table_tests tests/test_vulkan_main_material_table.cpp)
        target_link_libraries(dve_vulkan_main_material_table_tests PRIVATE dve::rhi_vulkan dve::render_bridge)
        add_test(NAME dve_vulkan_main_material_table_tests COMMAND dve_vulkan_main_material_table_tests)
        if(EXISTS "/usr/lib/chromium/vk_swiftshader_icd.json")
            set_tests_properties(dve_vulkan_buffer_tests dve_vulkan_graphics_tests dve_v193_vulkan_sprite_tests dve_depth_only_rhi_tests dve_vulkan_compute_tests dve_vulkan_camera_outputs_tests dve_vulkan_environment_lighting_tests dve_vulkan_live_environment_renderer_tests dve_vulkan_shadow_material_table_tests dve_vulkan_main_material_table_tests PROPERTIES
                ENVIRONMENT "VK_ICD_FILENAMES=/usr/lib/chromium/vk_swiftshader_icd.json;DVE_REQUIRE_VULKAN=1")
        endif()
    endif()

    add_executable(dve_simulation_validation_tests tests/test_simulation_validation.cpp)
    target_link_libraries(dve_simulation_validation_tests PRIVATE dve::core)
    add_test(NAME dve_simulation_validation_tests COMMAND dve_simulation_validation_tests)

    if(TARGET dve_simulation_device_validation)
        add_executable(dve_simulation_kernel_conformance_tests tests/test_simulation_kernel_conformance.cpp)
        target_link_libraries(dve_simulation_kernel_conformance_tests PRIVATE dve::simulation_device_validation)
        target_compile_definitions(dve_simulation_kernel_conformance_tests PRIVATE
            DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_simulation_kernel_conformance_tests COMMAND dve_simulation_kernel_conformance_tests)
    endif()

    if(TARGET dve_device_test_runner)
        add_test(NAME dve_device_test_runner_null
            COMMAND dve_device_test_runner --backend null --suite all
                --output ${CMAKE_BINARY_DIR}/validation_evidence_null
                --source-root ${CMAKE_CURRENT_SOURCE_DIR})
        if(UNIX AND EXISTS "/usr/lib/chromium/vk_swiftshader_icd.json" AND TARGET dve_rhi_vulkan)
            add_test(NAME dve_device_test_runner_swiftshader_production
                COMMAND dve_device_test_runner --backend vulkan --suite device
                    --output ${CMAKE_BINARY_DIR}/validation_evidence_swiftshader_production
                    --source-root ${CMAKE_CURRENT_SOURCE_DIR}
                    --require-backend --require-kernel-readback
                    --require-production-readback --require-timestamps)
            set_tests_properties(dve_device_test_runner_swiftshader_production PROPERTIES
                ENVIRONMENT "VK_ICD_FILENAMES=/usr/lib/chromium/vk_swiftshader_icd.json")
        endif()
    endif()

    add_executable(dve_material_parameter_collection_tests tests/test_material_parameter_collection.cpp)
    target_link_libraries(dve_material_parameter_collection_tests PRIVATE dve::core)
    add_test(NAME dve_material_parameter_collection_tests COMMAND dve_material_parameter_collection_tests)

    add_executable(dve_master_material_tests tests/test_master_material.cpp)
    target_link_libraries(dve_master_material_tests PRIVATE dve::core)
    add_test(NAME dve_master_material_tests COMMAND dve_master_material_tests)

    add_executable(dve_gpu_material_buffer_tests tests/test_gpu_material_buffer.cpp)
    target_link_libraries(dve_gpu_material_buffer_tests PRIVATE dve::core)
    target_compile_definitions(dve_gpu_material_buffer_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_gpu_material_buffer_tests COMMAND dve_gpu_material_buffer_tests)

    add_executable(dve_render_environment_tests tests/test_render_environment.cpp)
    target_link_libraries(dve_render_environment_tests PRIVATE dve::core)
    add_test(NAME dve_render_environment_tests COMMAND dve_render_environment_tests)

    if(DVE_BUILD_AUDIO_SYNTH)
        add_executable(dve_audio_synth_tests tests/test_audio_synth.cpp)
        target_link_libraries(dve_audio_synth_tests PRIVATE dve::audio_synth)
        target_compile_definitions(dve_audio_synth_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_audio_synth_tests COMMAND dve_audio_synth_tests)
        add_executable(dve_physical_model_tests tests/test_physical_model.cpp)
        target_link_libraries(dve_physical_model_tests PRIVATE dve::audio_synth)
        target_compile_definitions(dve_physical_model_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_physical_model_tests COMMAND dve_physical_model_tests)
        add_executable(dve_audio_sample_map_tests tests/test_audio_sample_map.cpp)
        target_link_libraries(dve_audio_sample_map_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_audio_sample_map_tests COMMAND dve_audio_sample_map_tests)
        add_executable(dve_sample_map_recipe_tests tests/test_sample_map_recipe.cpp)
        target_link_libraries(dve_sample_map_recipe_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_sample_map_recipe_tests COMMAND dve_sample_map_recipe_tests)
        add_executable(dve_audio_runtime_tests tests/test_audio_runtime.cpp)
        target_link_libraries(dve_audio_runtime_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_audio_runtime_tests COMMAND dve_audio_runtime_tests)
        add_executable(dve_audio_production_tests tests/test_audio_production.cpp)
        target_link_libraries(dve_audio_production_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_audio_production_tests COMMAND dve_audio_production_tests)
        add_executable(dve_audio_edit_tests tests/test_audio_edit.cpp)
        target_link_libraries(dve_audio_edit_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_audio_edit_tests COMMAND dve_audio_edit_tests)
        add_executable(dve_audio_workstation_tests tests/test_audio_workstation.cpp)
        target_link_libraries(dve_audio_workstation_tests PRIVATE dve::audio_synth)
        if(DVE_BUILD_EDITOR)
            target_link_libraries(dve_audio_workstation_tests PRIVATE dve::editor)
            target_compile_definitions(dve_audio_workstation_tests PRIVATE DVE_HAVE_EDITOR_TIMELINE=1)
        endif()
        add_test(NAME dve_audio_workstation_tests COMMAND dve_audio_workstation_tests)
        add_executable(dve_audio_wall_collapse_tests tests/test_audio_wall_collapse.cpp)
        target_link_libraries(dve_audio_wall_collapse_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_audio_wall_collapse_tests COMMAND dve_audio_wall_collapse_tests)
        add_executable(dve_audio_reflection_tests tests/test_audio_reflections.cpp)
        target_link_libraries(dve_audio_reflection_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_audio_reflection_tests COMMAND dve_audio_reflection_tests)
        add_executable(dve_audio_ambisonics_tests tests/test_audio_ambisonics.cpp)
        target_link_libraries(dve_audio_ambisonics_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_audio_ambisonics_tests COMMAND dve_audio_ambisonics_tests)
        add_executable(dve_physics_contact_sink_tests tests/test_physics_contact_sink.cpp)
        target_link_libraries(dve_physics_contact_sink_tests PRIVATE dve::audio_synth)
        add_test(NAME dve_physics_contact_sink_tests COMMAND dve_physics_contact_sink_tests)

        # Compile the production phonon.h adapter against a deterministic ABI-compatible shim.
        # This catches signature/lifetime/block-size regressions even when the external SDK
        # binaries are unavailable on the CI host.
        add_executable(dve_steam_audio_native_contract_tests
            tests/test_steam_audio_native.cpp
            tests/fake_steam_audio/fake_phonon.cpp
            src/audio/steam_audio_native.cpp)
        target_include_directories(dve_steam_audio_native_contract_tests PRIVATE
            include tests/fake_steam_audio)
        target_compile_features(dve_steam_audio_native_contract_tests PRIVATE cxx_std_23)
        target_link_libraries(dve_steam_audio_native_contract_tests PRIVATE dve::audio_synth)
        if(MSVC)
            target_compile_options(dve_steam_audio_native_contract_tests PRIVATE /W4 /permissive-)
        else()
            target_compile_options(dve_steam_audio_native_contract_tests PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        endif()
        add_test(NAME dve_steam_audio_native_contract_tests COMMAND dve_steam_audio_native_contract_tests)
        if(DVE_BUILD_BENCHMARK)
            add_executable(dve_steam_audio_source_pool_contract_bench
                apps/steam_audio_source_pool_contract_bench.cpp
                tests/fake_steam_audio/fake_phonon.cpp
                src/audio/steam_audio_native.cpp)
            target_include_directories(dve_steam_audio_source_pool_contract_bench PRIVATE
                include tests/fake_steam_audio)
            target_compile_features(dve_steam_audio_source_pool_contract_bench PRIVATE cxx_std_23)
            target_link_libraries(dve_steam_audio_source_pool_contract_bench PRIVATE dve::audio_synth)
            if(NOT MSVC)
                target_compile_options(dve_steam_audio_source_pool_contract_bench PRIVATE
                    -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
            endif()
        endif()
    endif()

    add_executable(dve_tests tests/test_main.cpp)
    target_link_libraries(dve_tests PRIVATE dve::core)
    add_test(NAME dve_tests COMMAND dve_tests)

    add_executable(dve_v234_physics_neutral_hooks_tests
        tests/test_physics_neutral_hooks_v234.cpp)
    target_link_libraries(dve_v234_physics_neutral_hooks_tests PRIVATE dve::core)
    add_test(NAME dve_v234_physics_neutral_hooks_tests
        COMMAND dve_v234_physics_neutral_hooks_tests)

    add_executable(dve_navigation_mesh_tests tests/test_navigation_mesh.cpp)
    target_link_libraries(dve_navigation_mesh_tests PRIVATE dve::core)
    add_test(NAME dve_navigation_mesh_tests COMMAND dve_navigation_mesh_tests)

    add_executable(dve_geometry_graph_tests tests/test_geometry_graph.cpp)
    target_link_libraries(dve_geometry_graph_tests PRIVATE dve::core)
    add_test(NAME dve_geometry_graph_tests COMMAND dve_geometry_graph_tests)

    add_executable(dve_v232_marionette_runtime_tests tests/test_marionette_runtime_v232.cpp)
    target_link_libraries(dve_v232_marionette_runtime_tests PRIVATE dve::core)
    add_test(NAME dve_v232_marionette_runtime_tests COMMAND dve_v232_marionette_runtime_tests)

    if(TARGET dve_asset_pipeline)
        add_executable(dve_heightmap_terrain_tests tests/test_heightmap_terrain.cpp)
        target_link_libraries(dve_heightmap_terrain_tests PRIVATE dve::asset_pipeline)
        add_test(NAME dve_heightmap_terrain_tests COMMAND dve_heightmap_terrain_tests)
    endif()

    add_executable(dve_gabor_volume_tests tests/test_gabor_volume.cpp)
    target_link_libraries(dve_gabor_volume_tests PRIVATE dve::core)
    add_test(NAME dve_gabor_volume_tests COMMAND dve_gabor_volume_tests)

    if(DVE_ENABLE_FLUODDITY)
        add_executable(dve_fluoddity_tests tests/test_fluoddity.cpp)
        target_link_libraries(dve_fluoddity_tests PRIVATE dve::core)
        target_compile_definitions(dve_fluoddity_tests PRIVATE DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_fluoddity_tests COMMAND dve_fluoddity_tests)

        add_executable(dve_runtime_fluoddity_tests tests/test_runtime_fluoddity.cpp)
        target_link_libraries(dve_runtime_fluoddity_tests PRIVATE dve::core)
        target_compile_definitions(dve_runtime_fluoddity_tests PRIVATE DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_runtime_fluoddity_tests COMMAND dve_runtime_fluoddity_tests)

        add_executable(dve_fluoddity_solver_tests tests/test_fluoddity_solver.cpp)
        target_link_libraries(dve_fluoddity_solver_tests PRIVATE dve::core)
        target_compile_definitions(dve_fluoddity_solver_tests PRIVATE DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_fluoddity_solver_tests COMMAND dve_fluoddity_solver_tests)
    endif()

    if(DVE_ENABLE_SOFT_BODIES)
        add_executable(dve_soft_body_tests tests/test_soft_body.cpp)
        target_link_libraries(dve_soft_body_tests PRIVATE dve::core)
        add_test(NAME dve_soft_body_tests COMMAND dve_soft_body_tests)
    endif()
    if(DVE_ENABLE_CPU_HAIR)
        add_executable(dve_cpu_hair_tests tests/test_cpu_hair.cpp)
        target_link_libraries(dve_cpu_hair_tests PRIVATE dve::core)
        add_test(NAME dve_cpu_hair_tests COMMAND dve_cpu_hair_tests)
        if(DVE_BUILD_RHI)
            add_executable(dve_cpu_hair_render_tests tests/test_cpu_hair_render.cpp)
            target_link_libraries(dve_cpu_hair_render_tests PRIVATE dve::render_bridge)
            add_test(NAME dve_cpu_hair_render_tests COMMAND dve_cpu_hair_render_tests)
        endif()
    endif()
    if(DVE_BUILD_EDITOR AND DVE_ENABLE_SOFT_BODIES AND DVE_ENABLE_BSPLINE_CLOTH)
        add_executable(dve_v189_deformable_runtime_tests tests/test_deformable_runtime_v189.cpp)
        target_link_libraries(dve_v189_deformable_runtime_tests PRIVATE dve::editor)
        add_test(NAME dve_v189_deformable_runtime_tests COMMAND dve_v189_deformable_runtime_tests)
    endif()

    if(DVE_ENABLE_VFX_PARTICLES)
        add_executable(dve_vfx_particle_graph_tests tests/test_vfx_particle_graph.cpp)
        target_link_libraries(dve_vfx_particle_graph_tests PRIVATE dve::core)
        add_test(NAME dve_vfx_particle_graph_tests COMMAND dve_vfx_particle_graph_tests)
    endif()

    if(DVE_ENABLE_PBF_LIQUID)
        add_executable(dve_pbf_liquid_tests tests/test_liquid_pbf.cpp)
        target_link_libraries(dve_pbf_liquid_tests PRIVATE dve::core)
        add_test(NAME dve_pbf_liquid_tests COMMAND dve_pbf_liquid_tests)
    endif()

    add_executable(dve_simulation_quality_tests tests/test_simulation_quality.cpp)
    target_link_libraries(dve_simulation_quality_tests PRIVATE dve::core)
    add_test(NAME dve_simulation_quality_tests COMMAND dve_simulation_quality_tests)

    if(DVE_ENABLE_SIMULATION_ENERGY_COMPILER)
        add_executable(dve_simulation_energy_tests tests/test_simulation_energy.cpp)
        target_link_libraries(dve_simulation_energy_tests PRIVATE dve::core)
        add_test(NAME dve_simulation_energy_tests COMMAND dve_simulation_energy_tests)
    endif()

    if(DVE_ENABLE_BSPLINE_CLOTH)
        add_executable(dve_bspline_cloth_tests tests/test_bspline_cloth.cpp)
        target_link_libraries(dve_bspline_cloth_tests PRIVATE dve::core)
        add_test(NAME dve_bspline_cloth_tests COMMAND dve_bspline_cloth_tests)
    endif()
    if(DVE_ENABLE_GRID_FLUIDS)
        add_executable(dve_grid_fluid_tests tests/test_grid_fluid.cpp)
        target_link_libraries(dve_grid_fluid_tests PRIVATE dve::core)
        add_test(NAME dve_grid_fluid_tests COMMAND dve_grid_fluid_tests)
    endif()
    if(DVE_ENABLE_FLIP_LIQUIDS)
        add_executable(dve_flip_liquid_tests tests/test_flip_liquid.cpp)
        target_link_libraries(dve_flip_liquid_tests PRIVATE dve::core)
        add_test(NAME dve_flip_liquid_tests COMMAND dve_flip_liquid_tests)
    endif()

    add_executable(dve_game_world_tests tests/test_game_world.cpp)
    target_link_libraries(dve_game_world_tests PRIVATE dve::core)
    add_test(NAME dve_game_world_tests COMMAND dve_game_world_tests)

    add_executable(dve_animation_tests tests/test_animation.cpp)
    target_link_libraries(dve_animation_tests PRIVATE dve::core)
    add_test(NAME dve_animation_tests COMMAND dve_animation_tests)

    add_executable(dve_v181_animation_ui_tests tests/test_animation_game_ui_v181.cpp)
    target_link_libraries(dve_v181_animation_ui_tests PRIVATE dve::core)
    add_test(NAME dve_v181_animation_ui_tests COMMAND dve_v181_animation_ui_tests)

    add_executable(dve_v182_control_rig_tests tests/test_control_rig_v182.cpp)
    target_link_libraries(dve_v182_control_rig_tests PRIVATE dve::core)
    add_test(NAME dve_v182_control_rig_tests COMMAND dve_v182_control_rig_tests)

    add_executable(dve_v183_control_rig_authoring_tests tests/test_control_rig_authoring_v183.cpp)
    target_link_libraries(dve_v183_control_rig_authoring_tests PRIVATE dve::core)
    add_test(NAME dve_v183_control_rig_authoring_tests COMMAND dve_v183_control_rig_authoring_tests)

    if(DVE_BUILD_EDITOR)
        add_executable(dve_v184_control_rig_editor_tests tests/test_control_rig_editor_v184.cpp)
        target_link_libraries(dve_v184_control_rig_editor_tests PRIVATE dve::editor)
        target_compile_definitions(dve_v184_control_rig_editor_tests PRIVATE DVE_V184_NATIVE_INTEGRATION_TEST=1)
        add_test(NAME dve_v184_control_rig_editor_tests COMMAND dve_v184_control_rig_editor_tests)

        add_executable(dve_v185_control_rig_asset_workflow_tests tests/test_control_rig_asset_workflow_v185.cpp)
        target_link_libraries(dve_v185_control_rig_asset_workflow_tests PRIVATE dve::editor)
        target_compile_definitions(dve_v185_control_rig_asset_workflow_tests PRIVATE DVE_V185_NATIVE_INTEGRATION_TEST=1)
        add_test(NAME dve_v185_control_rig_asset_workflow_tests COMMAND dve_v185_control_rig_asset_workflow_tests)

        add_executable(dve_v186_control_rig_inspector_tests tests/test_control_rig_inspector_v186.cpp)
        target_link_libraries(dve_v186_control_rig_inspector_tests PRIVATE dve::editor)
        add_test(NAME dve_v186_control_rig_inspector_tests COMMAND dve_v186_control_rig_inspector_tests)
    endif()

    add_executable(dve_v187_ragdoll_runtime_tests tests/test_ragdoll_runtime_v187.cpp)
    target_link_libraries(dve_v187_ragdoll_runtime_tests PRIVATE dve::core)
    target_compile_definitions(dve_v187_ragdoll_runtime_tests PRIVATE DVE_V187_GAMEWORLD_INTEGRATION_TEST=1)
    add_test(NAME dve_v187_ragdoll_runtime_tests COMMAND dve_v187_ragdoll_runtime_tests)

    add_executable(dve_v191_sprite2d_tests tests/test_sprite2d_v191.cpp)
    target_link_libraries(dve_v191_sprite2d_tests PRIVATE dve::core)
    add_test(NAME dve_v191_sprite2d_tests COMMAND dve_v191_sprite2d_tests)

    if(DVE_BUILD_EDITOR)
        add_executable(dve_v188_game_ui_tests tests/test_game_ui_v188.cpp)
        target_link_libraries(dve_v188_game_ui_tests PRIVATE dve::editor)
        add_test(NAME dve_v188_game_ui_tests COMMAND dve_v188_game_ui_tests)
    endif()

    add_executable(dve_gameplay_runtime_tests tests/test_gameplay_runtime.cpp)
    target_link_libraries(dve_gameplay_runtime_tests PRIVATE dve::core)
    add_test(NAME dve_gameplay_runtime_tests COMMAND dve_gameplay_runtime_tests)

    add_executable(dve_replication_contract_tests tests/test_replication_contract.cpp)
    target_link_libraries(dve_replication_contract_tests PRIVATE dve::core)
    add_test(NAME dve_replication_contract_tests COMMAND dve_replication_contract_tests)

    add_executable(dve_network_simulation_tests tests/test_network_simulation.cpp)
    target_link_libraries(dve_network_simulation_tests PRIVATE dve::core)
    add_test(NAME dve_network_simulation_tests COMMAND dve_network_simulation_tests)

    add_executable(dve_network_runtime_tests tests/test_network_runtime.cpp)
    target_link_libraries(dve_network_runtime_tests PRIVATE dve::core)
    add_test(NAME dve_network_runtime_tests COMMAND dve_network_runtime_tests)

    add_executable(dve_network_transport_tests tests/test_network_transport.cpp)
    target_link_libraries(dve_network_transport_tests PRIVATE dve::core)
    add_test(NAME dve_network_transport_tests COMMAND dve_network_transport_tests)

    add_executable(dve_udp_multiprocess_tests tests/test_udp_multiprocess.cpp)
    target_link_libraries(dve_udp_multiprocess_tests PRIVATE dve::core)
    add_test(NAME dve_udp_multiprocess_tests COMMAND dve_udp_multiprocess_tests)

    add_executable(dve_ai_assistant_tests tests/test_ai_assistant.cpp)
    target_link_libraries(dve_ai_assistant_tests PRIVATE dve::core)
    target_compile_definitions(dve_ai_assistant_tests PRIVATE DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_ai_assistant_tests COMMAND dve_ai_assistant_tests)

    add_executable(dve_live_editor_mcp_core_tests tests/test_live_editor_mcp_core.cpp)
    target_link_libraries(dve_live_editor_mcp_core_tests PRIVATE dve::core)
    add_test(NAME dve_live_editor_mcp_core_tests COMMAND dve_live_editor_mcp_core_tests)

    if(DVE_BUILD_EDITOR)
        add_executable(dve_live_editor_mcp_tests tests/test_live_editor_mcp.cpp)
        target_link_libraries(dve_live_editor_mcp_tests PRIVATE dve::editor)
        add_test(NAME dve_live_editor_mcp_tests COMMAND dve_live_editor_mcp_tests)
    endif()

    if(UNIX AND NOT APPLE)
        find_package(X11 QUIET)
        if(X11_FOUND)
            add_executable(dve_x11_header_compat_tests tests/test_x11_header_compat.cpp)
            target_include_directories(dve_x11_header_compat_tests PRIVATE ${X11_INCLUDE_DIR})
            target_link_libraries(dve_x11_header_compat_tests PRIVATE dve::core)
            add_test(NAME dve_x11_header_compat_tests COMMAND dve_x11_header_compat_tests)
        endif()
    endif()

    if(TARGET dve_asset_pipeline)
        add_executable(dve_asset_cooker_tests tests/test_asset_cooker.cpp)
        target_link_libraries(dve_asset_cooker_tests PRIVATE dve::asset_pipeline)
        target_compile_definitions(dve_asset_cooker_tests PRIVATE DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_asset_cooker_tests COMMAND dve_asset_cooker_tests)
    endif()

    add_executable(dve_text3d_tests tests/test_text3d.cpp)
    target_link_libraries(dve_text3d_tests PRIVATE dve::core)
    add_test(NAME dve_text3d_tests COMMAND dve_text3d_tests)

    if(TARGET dve_render_bridge)
        add_executable(dve_text3d_render_tests tests/test_text3d_render.cpp)
        target_link_libraries(dve_text3d_render_tests PRIVATE dve::render_bridge)
        add_test(NAME dve_text3d_render_tests COMMAND dve_text3d_render_tests)

        add_executable(dve_runtime_text3d_tests tests/test_runtime_text3d.cpp)
        target_link_libraries(dve_runtime_text3d_tests PRIVATE dve::render_bridge)
        add_test(NAME dve_runtime_text3d_tests COMMAND dve_runtime_text3d_tests)

        add_executable(dve_runtime_gabor_volume_tests tests/test_runtime_gabor_volume.cpp)
        target_link_libraries(dve_runtime_gabor_volume_tests PRIVATE dve::render_bridge)
        add_test(NAME dve_runtime_gabor_volume_tests COMMAND dve_runtime_gabor_volume_tests)

        if(DVE_ENABLE_FLUODDITY AND DVE_ENABLE_VFX_PARTICLES AND DVE_ENABLE_PBF_LIQUID AND DVE_ENABLE_FLIP_LIQUIDS)
            add_executable(dve_simulation_gpu_contract_tests tests/test_simulation_gpu_contracts.cpp)
            target_link_libraries(dve_simulation_gpu_contract_tests PRIVATE dve::render_bridge)
            add_test(NAME dve_simulation_gpu_contract_tests COMMAND dve_simulation_gpu_contract_tests)
        endif()
    endif()

    add_executable(dve_runtime_scene_tests tests/test_runtime_scene.cpp)
    target_link_libraries(dve_runtime_scene_tests PRIVATE dve::core)
    target_compile_definitions(dve_runtime_scene_tests PRIVATE DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_runtime_scene_tests COMMAND dve_runtime_scene_tests)

    if(DVE_BUILD_EDITOR)
        add_executable(dve_editor_tests tests/test_editor.cpp)
        target_link_libraries(dve_editor_tests PRIVATE dve::editor)
        target_compile_definitions(dve_editor_tests PRIVATE DVE_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_editor_tests COMMAND dve_editor_tests)

        add_executable(dve_editor_play_session_tests tests/test_editor_play_session.cpp)
        target_link_libraries(dve_editor_play_session_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_play_session_tests COMMAND dve_editor_play_session_tests)
        add_executable(dve_editor_asset_browser_tests tests/test_editor_asset_browser.cpp)
        target_link_libraries(dve_editor_asset_browser_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_asset_browser_tests COMMAND dve_editor_asset_browser_tests)
        add_executable(dve_scene_composition_tests tests/test_scene_composition.cpp)
        target_link_libraries(dve_scene_composition_tests PRIVATE dve::editor)
        add_test(NAME dve_scene_composition_tests COMMAND dve_scene_composition_tests)

        add_executable(dve_editor_text3d_tests tests/test_editor_text3d.cpp)
        target_link_libraries(dve_editor_text3d_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_text3d_tests COMMAND dve_editor_text3d_tests)

        add_executable(dve_editor_gabor_volume_tests tests/test_editor_gabor_volume.cpp)
        target_link_libraries(dve_editor_gabor_volume_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_gabor_volume_tests COMMAND dve_editor_gabor_volume_tests)

        add_executable(dve_editor_settings_tests tests/test_editor_settings.cpp)
        target_link_libraries(dve_editor_settings_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_settings_tests COMMAND dve_editor_settings_tests)
        add_executable(dve_editor_shortcuts_tests tests/test_editor_shortcuts.cpp)
        target_link_libraries(dve_editor_shortcuts_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_shortcuts_tests COMMAND dve_editor_shortcuts_tests)
        add_executable(dve_menu_command_center_v220_tests tests/test_menu_command_center_v220.cpp)
        target_link_libraries(dve_menu_command_center_v220_tests PRIVATE dve::editor)
        add_test(NAME dve_menu_command_center_v220_tests COMMAND dve_menu_command_center_v220_tests)
        add_executable(dve_editor_camera_sequencer_tests tests/test_editor_camera_sequencer.cpp)
        target_link_libraries(dve_editor_camera_sequencer_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_camera_sequencer_tests COMMAND dve_editor_camera_sequencer_tests)
        add_executable(dve_v229_cinematic_camera_ux_tests tests/test_v229_cinematic_camera_ux.cpp)
        target_link_libraries(dve_v229_cinematic_camera_ux_tests PRIVATE dve::editor)
        add_test(NAME dve_v229_cinematic_camera_ux_tests COMMAND dve_v229_cinematic_camera_ux_tests)

        add_executable(dve_editor_synth_tests tests/test_editor_synth.cpp)
        target_link_libraries(dve_editor_synth_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_synth_tests COMMAND dve_editor_synth_tests)
        add_executable(dve_editor_audio_tests tests/test_editor_audio.cpp)
        target_link_libraries(dve_editor_audio_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_audio_tests COMMAND dve_editor_audio_tests)
        add_executable(dve_editor_chiptune_tests tests/test_editor_chiptune_v217.cpp)
        target_link_libraries(dve_editor_chiptune_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_chiptune_tests COMMAND dve_editor_chiptune_tests)
        add_executable(dve_editor_render3d_diagnostics_v223_tests tests/test_editor_render3d_diagnostics_v223.cpp)
        target_link_libraries(dve_editor_render3d_diagnostics_v223_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_render3d_diagnostics_v223_tests COMMAND dve_editor_render3d_diagnostics_v223_tests)
        add_executable(dve_editor_voxel_material_policy_v224_tests tests/test_editor_voxel_material_policy_v224.cpp)
        target_link_libraries(dve_editor_voxel_material_policy_v224_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_voxel_material_policy_v224_tests COMMAND dve_editor_voxel_material_policy_v224_tests)
        add_executable(dve_editor_sprite_pixel_rig_v222_tests tests/test_editor_sprite_pixel_rig_v222.cpp)
        target_link_libraries(dve_editor_sprite_pixel_rig_v222_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_sprite_pixel_rig_v222_tests COMMAND dve_editor_sprite_pixel_rig_v222_tests)
        add_executable(dve_editor_sprite_level_v218_tests tests/test_editor_sprite_level_v218.cpp)
        target_link_libraries(dve_editor_sprite_level_v218_tests PRIVATE dve::editor)
        target_compile_definitions(dve_editor_sprite_level_v218_tests PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_editor_sprite_level_v218_tests COMMAND dve_editor_sprite_level_v218_tests)
        add_executable(dve_editor_sprite_diagnostics_tile_world_v219_tests
            tests/test_editor_sprite_diagnostics_tile_world_v219.cpp)
        target_link_libraries(dve_editor_sprite_diagnostics_tile_world_v219_tests PRIVATE dve::editor)
        target_compile_definitions(dve_editor_sprite_diagnostics_tile_world_v219_tests
            PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME dve_editor_sprite_diagnostics_tile_world_v219_tests
            COMMAND dve_editor_sprite_diagnostics_tile_world_v219_tests)
        add_executable(dve_editor_audio_event_tests tests/test_editor_audio_event.cpp)
        target_link_libraries(dve_editor_audio_event_tests PRIVATE dve::editor)
        add_test(NAME dve_editor_audio_event_tests COMMAND dve_editor_audio_event_tests)
        if(TARGET dve_native_editor_x11)
            find_program(DVE_XVFB_RUN xvfb-run)
            if(DVE_XVFB_RUN)
                add_test(NAME dve_native_editor_smoke
                    COMMAND ${DVE_XVFB_RUN} -a $<TARGET_FILE:dve_native_editor_x11>
                        --smoke --screenshot ${CMAKE_BINARY_DIR}/native_editor_smoke.ppm)
            endif()
        endif()
    endif()
endif()

if(DVE_BUILD_BENCHMARK)
    if(DVE_BUILD_AUDIO_SYNTH)
        add_executable(dve_audio_synth_bench apps/audio_synth_bench.cpp)
        target_link_libraries(dve_audio_synth_bench PRIVATE dve::audio_synth)
        add_executable(dve_audio_synth_typical_bench apps/audio_synth_typical_bench.cpp)
        target_link_libraries(dve_audio_synth_typical_bench PRIVATE dve::audio_synth)
        add_executable(dve_audio_synth_sample_granular_bench apps/audio_synth_sample_granular_bench.cpp)
        target_link_libraries(dve_audio_synth_sample_granular_bench PRIVATE dve::audio_synth)
        add_executable(dve_physical_preset_audit apps/physical_preset_audit.cpp)
        target_link_libraries(dve_physical_preset_audit PRIVATE dve::audio_synth)
        add_executable(dve_physical_preset_showcase apps/physical_preset_showcase.cpp)
        target_link_libraries(dve_physical_preset_showcase PRIVATE dve::audio_synth)
        add_executable(dve_hybrid_synth_demo apps/hybrid_synth_demo.cpp)
        target_link_libraries(dve_hybrid_synth_demo PRIVATE dve::audio_synth)
        add_executable(dve_sample_granular_synth_demo apps/sample_granular_synth_demo.cpp)
        target_link_libraries(dve_sample_granular_synth_demo PRIVATE dve::audio_synth)
        add_executable(dve_audio_runtime_bench apps/audio_runtime_bench.cpp)
        target_link_libraries(dve_audio_runtime_bench PRIVATE dve::audio_synth)
        add_executable(dve_audio_stream_bench apps/audio_stream_bench.cpp)
        target_link_libraries(dve_audio_stream_bench PRIVATE dve::audio_synth)
    endif()

    add_executable(dve_bench apps/voxel_bench.cpp)
    target_link_libraries(dve_bench PRIVATE dve::core)
    if(DVE_ENABLE_CPU_HAIR)
        add_executable(dve_cpu_hair_bench apps/cpu_hair_bench.cpp)
        target_link_libraries(dve_cpu_hair_bench PRIVATE dve::core)
        if(DVE_BUILD_RHI)
            add_executable(dve_cpu_hair_render_bench apps/cpu_hair_render_bench.cpp)
            target_link_libraries(dve_cpu_hair_render_bench PRIVATE dve::render_bridge)
        endif()
    endif()

    add_executable(dve_incremental_demo apps/incremental_split_demo.cpp)
    add_executable(dve_adversarial_bench apps/adversarial_bench.cpp)
    add_executable(dve_player_query_bench apps/player_query_bench.cpp)
    add_executable(dve_fragment_bench apps/fragment_bench.cpp)
    add_executable(dve_primary_trace_bench apps/primary_trace_bench.cpp)
    add_executable(dve_asset_cooker_bench apps/asset_cooker_bench.cpp)
    add_executable(dve_runtime_scene_bench apps/runtime_scene_bench.cpp)
    add_executable(dve_runtime_scene_scale_bench apps/runtime_scene_scale_bench.cpp)
    add_executable(dve_runtime_scene_authored_bench apps/runtime_scene_authored_bench.cpp)
    add_executable(dve_runtime_brickmap_publication_bench apps/runtime_brickmap_publication_bench.cpp)
    if(DVE_BUILD_EDITOR)
        add_executable(dve_editor_interaction_bench apps/editor_interaction_bench.cpp)
        target_link_libraries(dve_editor_interaction_bench PRIVATE dve::editor)
    endif()
    target_link_libraries(dve_adversarial_bench PRIVATE dve::core)
    target_link_libraries(dve_player_query_bench PRIVATE dve::core)
    target_link_libraries(dve_fragment_bench PRIVATE dve::core)
    target_link_libraries(dve_primary_trace_bench PRIVATE dve::core)
    target_link_libraries(dve_asset_cooker_bench PRIVATE dve::asset_pipeline)
    target_link_libraries(dve_runtime_scene_bench PRIVATE dve::core)
    target_link_libraries(dve_runtime_scene_scale_bench PRIVATE dve::core)
    target_link_libraries(dve_runtime_scene_authored_bench PRIVATE dve::core)
    target_link_libraries(dve_runtime_brickmap_publication_bench PRIVATE dve::core)
    target_compile_definitions(dve_runtime_scene_bench PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(dve_runtime_scene_scale_bench PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(dve_runtime_scene_authored_bench PRIVATE DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(dve_incremental_demo PRIVATE dve::core)

    if(DVE_BUILD_TESTS)
        add_test(NAME dve_incremental_demo COMMAND dve_incremental_demo)
    endif()
endif()


if(DVE_BUILD_TESTS AND DVE_BUILD_RHI)
    add_executable(dve_material_authoring_tests tests/test_material_authoring.cpp)
    target_link_libraries(dve_material_authoring_tests PRIVATE dve::core)
    add_test(NAME dve_material_authoring_tests COMMAND dve_material_authoring_tests)

    add_executable(dve_material_mapping_tests tests/test_material_mapping.cpp)
    target_link_libraries(dve_material_mapping_tests PRIVATE dve::core)
    add_test(NAME dve_material_mapping_tests COMMAND dve_material_mapping_tests)

    add_executable(dve_material_layers_tests tests/test_material_layers.cpp)
    target_link_libraries(dve_material_layers_tests PRIVATE dve::core)
    add_test(NAME dve_material_layers_tests COMMAND dve_material_layers_tests)

    add_executable(dve_destruction_material_tests tests/test_destruction_material.cpp)
    target_link_libraries(dve_destruction_material_tests PRIVATE dve::core)
    add_test(NAME dve_destruction_material_tests COMMAND dve_destruction_material_tests)

    add_executable(dve_material_decal_tests tests/test_material_decal.cpp)
    target_link_libraries(dve_material_decal_tests PRIVATE dve::core)
    add_test(NAME dve_material_decal_tests COMMAND dve_material_decal_tests)

    add_executable(dve_material_displacement_tests tests/test_material_displacement.cpp)
    target_link_libraries(dve_material_displacement_tests PRIVATE dve::core)
    add_test(NAME dve_material_displacement_tests COMMAND dve_material_displacement_tests)

    add_executable(dve_environment_lighting_tests tests/test_environment_lighting.cpp)
    target_link_libraries(dve_environment_lighting_tests PRIVATE dve::core)
    add_test(NAME dve_environment_lighting_tests COMMAND dve_environment_lighting_tests)
    add_executable(dve_v2292_lighting_rendering_fixes_tests tests/test_lighting_rendering_fixes_v2292.cpp)
    target_link_libraries(dve_v2292_lighting_rendering_fixes_tests PRIVATE dve::render_bridge)
    target_compile_definitions(dve_v2292_lighting_rendering_fixes_tests PRIVATE
        DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_v2292_lighting_rendering_fixes_tests COMMAND dve_v2292_lighting_rendering_fixes_tests)
    add_executable(dve_v2293_shader_gi_fixes_tests tests/test_shader_gi_fixes_v2293.cpp)
    target_link_libraries(dve_v2293_shader_gi_fixes_tests PRIVATE dve::render_bridge)
    target_compile_definitions(dve_v2293_shader_gi_fixes_tests PRIVATE
        DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_v2293_shader_gi_fixes_tests COMMAND dve_v2293_shader_gi_fixes_tests)
    add_executable(dve_v2294_shadow_ui_fixes_tests tests/test_shadow_ui_fixes_v2294.cpp)
    target_link_libraries(dve_v2294_shadow_ui_fixes_tests PRIVATE dve::render_bridge)
    target_compile_definitions(dve_v2294_shadow_ui_fixes_tests PRIVATE
        DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_v2294_shadow_ui_fixes_tests COMMAND dve_v2294_shadow_ui_fixes_tests)
    add_executable(dve_polygon_geometry_tests tests/test_polygon_geometry.cpp)
    target_link_libraries(dve_polygon_geometry_tests PRIVATE dve::core dve::render_bridge dve::rhi)
    add_test(NAME dve_polygon_geometry_tests COMMAND dve_polygon_geometry_tests)
    add_executable(dve_polygon_rendering_tests tests/test_polygon_rendering.cpp)
    target_link_libraries(dve_polygon_rendering_tests PRIVATE dve::core dve::render_bridge dve::rhi)
    add_test(NAME dve_polygon_rendering_tests COMMAND dve_polygon_rendering_tests)
    add_executable(dve_v192_sprite_rhi_tests tests/test_sprite_rhi_v192.cpp)
    target_link_libraries(dve_v192_sprite_rhi_tests PRIVATE dve::render_bridge)
    target_compile_definitions(dve_v192_sprite_rhi_tests PRIVATE
        DVE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME dve_v192_sprite_rhi_tests COMMAND dve_v192_sprite_rhi_tests)
    add_executable(dve_v194_sprite_authoring_tests tests/test_sprite_authoring_v194.cpp)
    target_link_libraries(dve_v194_sprite_authoring_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_v194_sprite_authoring_tests COMMAND dve_v194_sprite_authoring_tests)
    add_executable(dve_v204_physics2d_tests tests/test_physics2d_v204.cpp)
    target_link_libraries(dve_v204_physics2d_tests PRIVATE dve::core)
    add_test(NAME dve_v204_physics2d_tests COMMAND dve_v204_physics2d_tests)
    add_executable(dve_texture_residency_tests tests/test_texture_residency.cpp)
    target_link_libraries(dve_texture_residency_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_texture_residency_tests COMMAND dve_texture_residency_tests)

    add_executable(dve_hybrid_reference_rendering_tests tests/test_hybrid_reference_rendering.cpp)
    target_link_libraries(dve_hybrid_reference_rendering_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_hybrid_reference_rendering_tests COMMAND dve_hybrid_reference_rendering_tests)
    add_executable(dve_voxel_lighting_plan_tests tests/test_voxel_lighting_plan.cpp)
    target_link_libraries(dve_voxel_lighting_plan_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_voxel_lighting_plan_tests COMMAND dve_voxel_lighting_plan_tests)

    add_executable(dve_cascaded_shadow_map_tests tests/test_cascaded_shadow_map.cpp)
    target_link_libraries(dve_cascaded_shadow_map_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_cascaded_shadow_map_tests COMMAND dve_cascaded_shadow_map_tests)

    add_executable(dve_production_environment_lighting_tests tests/test_production_environment_lighting.cpp)
    target_link_libraries(dve_production_environment_lighting_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_production_environment_lighting_tests COMMAND dve_production_environment_lighting_tests)

    add_executable(dve_live_environment_renderer_tests tests/test_live_environment_renderer.cpp)
    target_link_libraries(dve_live_environment_renderer_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_live_environment_renderer_tests COMMAND dve_live_environment_renderer_tests)
    add_executable(dve_shadow_material_table_tests tests/test_shadow_material_table.cpp)
    target_link_libraries(dve_shadow_material_table_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_shadow_material_table_tests COMMAND dve_shadow_material_table_tests)
    add_executable(dve_main_material_table_tests tests/test_main_material_table.cpp)
    target_link_libraries(dve_main_material_table_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_main_material_table_tests COMMAND dve_main_material_table_tests)
    add_executable(dve_shared_material_residency_tests tests/test_shared_material_residency.cpp)
    target_link_libraries(dve_shared_material_residency_tests PRIVATE dve::render_bridge)
    add_test(NAME dve_shared_material_residency_tests COMMAND dve_shared_material_residency_tests)
endif()
