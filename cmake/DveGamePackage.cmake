# dve_add_game_package(): a build target that turns a game project into a shippable folder
# (packaging Phase 4, plan §4.5). Works in the DVE tree and from an installed dve package
# (dveConfig.cmake includes this file).
#
#   dve_add_game_package(<target>
#       PROJECT_DIR <game project with game.dvegame>
#       OUTPUT_DIR  <folder to create; replaced on every run>
#       [NAME <Game>]            executable/folder name (default: game.dvegame name, sanitized)
#       [TGZ]                    also write <OUTPUT_DIR>/../<Game>-<version>-linux-x86_64.tar.gz
#       [ZIP]                    also write <OUTPUT_DIR>/../<Game>-<version>-windows-x86_64.zip
#                                (Windows: <Game>.exe with its DLLs next to it)
#       [VERIFY]                 run the packaged game for 2 headless frames afterwards
#       [ALL]                    build it with the default target
#       [RUNTIME_PREFIX <dir>]   installed dve Runtime (default: this package's prefix; in the
#                                DVE tree: the Runtime/RuntimeDeps components of this build)
#       [MATERIALS <file.dvematerials>]  material library for editor-scene export
#       [EXTRA_ARGS ...])        passed to dve_package_game
#
# The work is done by dve_package_game (scripts/dve_package_game.py): export editor scenes with
# dve_export_scene, pack with dve_pack (editor-only data such as .autosave/ excluded), copy the
# Runtime component (dve_player renamed to <Game>, lib/dve), THIRD_PARTY_NOTICES and
# build-info.json.
include_guard(GLOBAL)

function(dve_add_game_package target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "TGZ;ZIP;VERIFY;ALL"
        "PROJECT_DIR;OUTPUT_DIR;NAME;RUNTIME_PREFIX;MATERIALS" "EXTRA_ARGS")
    if(NOT arg_PROJECT_DIR OR NOT arg_OUTPUT_DIR)
        message(FATAL_ERROR "dve_add_game_package(${target}): PROJECT_DIR and OUTPUT_DIR are required")
    endif()
    find_program(DVE_PYTHON3_EXECUTABLE python3 REQUIRED)
    set(command "${DVE_PYTHON3_EXECUTABLE}")
    set(depends "")
    if(TARGET dve_player AND TARGET dve_pack AND DEFINED DVE_SOURCE_TREE_DIR)
        # In the DVE tree: this build's tools, and its Runtime components installed by the script.
        list(APPEND command "${DVE_SOURCE_TREE_DIR}/scripts/dve_package_game.py"
            --dve-pack "$<TARGET_FILE:dve_pack>")
        list(APPEND depends dve_player dve_pack)
        if(TARGET dve_export_scene)
            list(APPEND command --dve-export-scene "$<TARGET_FILE:dve_export_scene>")
            list(APPEND depends dve_export_scene)
        endif()
        if(TARGET dve_third_party_notices)
            list(APPEND depends dve_third_party_notices)
        endif()
        if(arg_RUNTIME_PREFIX)
            list(APPEND command --runtime-prefix "${arg_RUNTIME_PREFIX}")
        else()
            list(APPEND command --build-dir "${PROJECT_BINARY_DIR}" --cmake "${CMAKE_COMMAND}")
            get_property(_dve_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
            if(_dve_multi_config)
                # Visual Studio / Ninja Multi-Config: install the configuration being built.
                list(APPEND command --config "$<CONFIG>")
            endif()
        endif()
    else()
        # From an installed package: <prefix>/bin/dve_package_game and the Runtime next to it.
        if(NOT DEFINED dve_INSTALL_PREFIX)
            message(FATAL_ERROR "dve_add_game_package(${target}): find_package(dve) first")
        endif()
        set(script "${dve_INSTALL_PREFIX}/${dve_INSTALL_BINDIR}/dve_package_game")
        if(NOT EXISTS "${script}")
            message(FATAL_ERROR "dve_add_game_package(${target}): ${script} not found (install the dve Tools component)")
        endif()
        list(APPEND command "${script}")
        if(arg_RUNTIME_PREFIX)
            list(APPEND command --runtime-prefix "${arg_RUNTIME_PREFIX}")
        else()
            list(APPEND command --runtime-prefix "${dve_INSTALL_PREFIX}")
        endif()
    endif()
    list(APPEND command --project "${arg_PROJECT_DIR}" --output "${arg_OUTPUT_DIR}")
    if(arg_NAME)
        list(APPEND command --name "${arg_NAME}")
    endif()
    if(arg_MATERIALS)
        list(APPEND command --materials "${arg_MATERIALS}")
    endif()
    if(arg_TGZ)
        list(APPEND command --tgz)
    endif()
    if(arg_ZIP)
        list(APPEND command --zip)
    endif()
    if(arg_VERIFY)
        list(APPEND command --verify)
    endif()
    list(APPEND command ${arg_EXTRA_ARGS})
    set(all "")
    if(arg_ALL)
        set(all ALL)
    endif()
    add_custom_target(${target} ${all}
        COMMAND ${command}
        DEPENDS ${depends}
        COMMENT "Packaging game ${arg_PROJECT_DIR} -> ${arg_OUTPUT_DIR}"
        USES_TERMINAL
        VERBATIM)
endfunction()
