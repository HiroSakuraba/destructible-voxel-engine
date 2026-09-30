# Install-tree test (packaging plan §6): `cmake --install` every component into a scratch
# prefix, check the file list, the $ORIGIN RPATH and the bundled libraries, and run the
# installed dve_player on the sample pak from outside the build tree with nothing on
# LD_LIBRARY_PATH. The prefix is the dve_install_prefix fixture for dve_package_consumer_test.
#   SETTINGS   dve_install_test_settings.cmake from the build folder
#   PREFIX     scratch install prefix (recreated)
#   WORK_DIR   scratch folder
#   PROJECT    tests/data/player_sample (packed again with the installed dve_pack)
#   PAK        sample pak built by the dve_player_sample_pack fixture (when dve_player exists)
#   BUILD_PLAYER  build-tree dve_player, whose framebuffer hash the installed one must match
#   EXPECT     golden hashes for this build (optional)
cmake_minimum_required(VERSION 3.24)
foreach(required SETTINGS PREFIX WORK_DIR)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
include("${SETTINGS}")
file(REMOVE_RECURSE "${PREFIX}" "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set(problems "")
macro(problem text)
    list(APPEND problems "${text}")
endmacro()

foreach(component IN LISTS DVE_INSTALLED_COMPONENTS)
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${DVE_BINARY_DIR}" --prefix "${PREFIX}"
            --component "${component}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "cmake --install --component ${component} failed (${result}):\n${output}\n${errors}")
    endif()
endforeach()
file(GLOB_RECURSE installed RELATIVE "${PREFIX}" LIST_DIRECTORIES false "${PREFIX}/*")
list(LENGTH installed installed_count)
message(STATUS "installed ${installed_count} files for components: ${DVE_INSTALLED_COMPONENTS}")

# Expected files per component.
set(expected "")
foreach(executable IN LISTS DVE_INSTALLED_EXECUTABLES)
    list(APPEND expected "${DVE_BINDIR}/${executable}")
endforeach()
if("Development" IN_LIST DVE_INSTALLED_COMPONENTS)
    list(APPEND expected
        "${DVE_INCLUDEDIR}/dve/version.hpp"
        "${DVE_INCLUDEDIR}/dve/build_config.hpp"
        "${DVE_INCLUDEDIR}/dve/game_world.hpp"
        "${DVE_INCLUDEDIR}/dve/player/player_app.hpp"
        "${DVE_LIBDIR}/cmake/dve/dveConfig.cmake"
        "${DVE_LIBDIR}/cmake/dve/dveConfigVersion.cmake"
        "${DVE_LIBDIR}/cmake/dve/dveTargets.cmake")
    foreach(library IN LISTS DVE_EXPORTED_LIBRARIES)
        list(APPEND expected "${DVE_LIBDIR}/lib${library}.a")
    endforeach()
endif()
if("Editor" IN_LIST DVE_INSTALLED_COMPONENTS)
    list(APPEND expected "${DVE_DATADIR}/dve/assets/chiptune" "${DVE_DATADIR}/dve/assets/audio/presets")
endif()
# The engine's MIT license in every package (cmake/DveLicense.cmake): LICENSE and the Debian
# copyright file in share/doc/dve-<group>/.
file(READ "${DVE_SOURCE_DIR}/LICENSE" source_license)
foreach(component Runtime Editor Tools Development)
    if(component IN_LIST DVE_INSTALLED_COMPONENTS)
        string(TOLOWER "${component}" lower)
        if(lower STREQUAL "development")
            set(lower dev)
        endif()
        list(APPEND expected "${DVE_DATADIR}/doc/dve-${lower}/LICENSE" "${DVE_DATADIR}/doc/dve-${lower}/copyright")
        if(EXISTS "${PREFIX}/${DVE_DATADIR}/doc/dve-${lower}/LICENSE")
            file(READ "${PREFIX}/${DVE_DATADIR}/doc/dve-${lower}/LICENSE" installed_license)
            if(NOT installed_license STREQUAL source_license)
                problem("${DVE_DATADIR}/doc/dve-${lower}/LICENSE differs from the root LICENSE")
            endif()
        endif()
    endif()
endforeach()

# THIRD_PARTY_NOTICES per installed component (Phase 4).
set(notice_files "")
foreach(component IN LISTS DVE_NOTICES_COMPONENTS)
    if(component IN_LIST DVE_INSTALLED_COMPONENTS)
        string(TOLOWER "${component}" lower)
        if(lower STREQUAL "development")
            set(lower dev)
        endif()
        list(APPEND expected "${DVE_NOTICES_INSTALL_DIR}/THIRD_PARTY_NOTICES-${lower}.txt")
        list(APPEND notice_files "${PREFIX}/${DVE_NOTICES_INSTALL_DIR}/THIRD_PARTY_NOTICES-${lower}.txt")
    endif()
endforeach()
foreach(path IN LISTS expected)
    if(NOT EXISTS "${PREFIX}/${path}")
        problem("missing ${path}")
    endif()
endforeach()

# D8: no sample maps in any installed component unless explicitly enabled.
if(NOT DVE_SAMPLE_MAPS)
    foreach(path IN LISTS installed)
        if(path MATCHES "sample_maps")
            problem("sample map installed (D8): ${path}")
        endif()
    endforeach()
endif()

# The exported package must be relocatable: no build- or source-tree paths.
if(EXISTS "${PREFIX}/${DVE_LIBDIR}/cmake/dve")
    file(GLOB package_files "${PREFIX}/${DVE_LIBDIR}/cmake/dve/*.cmake")
    foreach(file IN LISTS package_files)
        file(READ "${file}" text)
        foreach(tree "${DVE_SOURCE_DIR}" "${DVE_BINARY_DIR}")
            string(FIND "${text}" "${tree}" at)
            if(NOT at EQUAL -1)
                problem("${file} references ${tree}")
            endif()
        endforeach()
    endforeach()
    # Consumers get no third-party include directories, so public headers must not include
    # third-party headers (SDL, Jolt, Lua, RtMidi, manifold, Box2D/3D, libpng/jpeg, Steam Audio).
    file(GLOB_RECURSE headers "${PREFIX}/${DVE_INCLUDEDIR}/dve/*.hpp")
    foreach(header IN LISTS headers)
        file(STRINGS "${header}" includes REGEX "^[ \t]*#[ \t]*include[ \t]*[<\"](SDL3|Jolt|lua|lauxlib|lualib|RtMidi|manifold|box2d|box3d|png|jpeglib|phonon)")
        if(includes)
            problem("public header ${header} includes a third-party header: ${includes}")
        endif()
    endforeach()
endif()

# RPATH and dependency resolution of every installed executable.
find_program(READELF readelf)
find_program(LDD ldd)
set(bundle "${PREFIX}/${DVE_BUNDLEDIR}")
set(bundled "")
if(EXISTS "${bundle}")
    file(GLOB bundled RELATIVE "${bundle}" "${bundle}/*")
endif()
list(LENGTH bundled bundled_count)
message(STATUS "bundled ${bundled_count} files in ${DVE_BUNDLEDIR}: ${bundled}")
foreach(executable IN LISTS DVE_INSTALLED_EXECUTABLES)
    set(binary "${PREFIX}/${DVE_BINDIR}/${executable}")
    if(NOT EXISTS "${binary}")
        continue()
    endif()
    if(READELF)
        execute_process(COMMAND "${READELF}" -d "${binary}" OUTPUT_VARIABLE dynamic)
        if(NOT dynamic MATCHES "\\(RPATH\\)[^\n]*\\$ORIGIN/\\.\\./${DVE_BUNDLEDIR}")
            problem("${executable}: no DT_RPATH with $ORIGIN/../${DVE_BUNDLEDIR}:\n${dynamic}")
        endif()
    endif()
    if(LDD)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${LDD}" "${binary}"
            OUTPUT_VARIABLE resolved ERROR_VARIABLE resolved_errors)
        string(REPLACE "\n" ";" lines "${resolved}")
        foreach(line IN LISTS lines)
            if(line MATCHES "not found")
                problem("${executable}: unresolved dependency: ${line}")
            elseif(line MATCHES "^[ \t]*([^ \t]+) => ([^ \t]+)")
                set(name "${CMAKE_MATCH_1}")
                set(path "${CMAKE_MATCH_2}")
                string(FIND "${path}" "${PREFIX}/" in_prefix)
                if(in_prefix EQUAL 0)
                    continue()
                endif()
                set(system OFF)
                foreach(pattern IN LISTS DVE_SYSTEM_EXCLUDES)
                    if(name MATCHES "${pattern}")
                        set(system ON)
                    endif()
                endforeach()
                if(NOT system AND DVE_BUNDLE_DEPENDENCIES)
                    problem("${executable}: ${name} resolves outside the prefix (${path}) and is not a system library")
                endif()
            endif()
        endforeach()
    endif()
endforeach()

# Every installed notices file starts with the engine's MIT license.
foreach(file IN LISTS notice_files)
    if(EXISTS "${file}")
        file(READ "${file}" text)
        if(NOT text MATCHES "is licensed under the MIT License" OR NOT text MATCHES "Copyright \\(c\\) 2026 Benjamin Schulz"
                OR text MATCHES "NO LICENSE")
            problem("${file} does not state the engine's MIT license")
        endif()
    endif()
endforeach()

# Every bundled shared library is covered by the installed notices.
if(notice_files AND DVE_PYTHON3 AND EXISTS "${bundle}")
    set(all_notices "${WORK_DIR}/all_notices.txt")
    file(WRITE "${all_notices}" "")
    foreach(file IN LISTS notice_files)
        if(EXISTS "${file}")
            file(READ "${file}" text)
            file(APPEND "${all_notices}" "${text}")
        endif()
    endforeach()
    execute_process(COMMAND "${DVE_PYTHON3}" "${DVE_NOTICES_TOOL}" --verify-dir "${bundle}" --notices "${all_notices}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        problem("installed THIRD_PARTY_NOTICES do not cover ${DVE_BUNDLEDIR}:\n${output}${errors}")
    endif()
endif()

set(ENV_ARGS --unset=LD_LIBRARY_PATH SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)
set(hash_args --frames 30 --fixed-dt 0.016666668 --render-size 480x270 --threads 4 --size 960x540 --hash)
function(hash_of text out)
    if(text MATCHES "framebuffer_fnv=([0-9a-f]+)")
        set(${out} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out} "" PARENT_SCOPE)
    endif()
endfunction()

# The installed player runs from its install location, from an unrelated working directory.
if(PAK AND EXISTS "${PREFIX}/${DVE_BINDIR}/dve_player")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} "${PREFIX}/${DVE_BINDIR}/dve_player" --version
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0 OR NOT output MATCHES "^dve_player ${DVE_VERSION}")
        problem("installed dve_player --version: ${result} ${output}${errors}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} "${PREFIX}/${DVE_BINDIR}/dve_player"
            --pak "${PAK}" ${hash_args}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE installed_output ERROR_VARIABLE installed_errors TIMEOUT 600)
    if(NOT result EQUAL 0)
        problem("installed dve_player failed (${result}):\n${installed_output}\n${installed_errors}")
    endif()
    hash_of("${installed_output}" installed_hash)
    if(BUILD_PLAYER)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} "${BUILD_PLAYER}" --pak "${PAK}" ${hash_args}
            WORKING_DIRECTORY "${WORK_DIR}"
            RESULT_VARIABLE result OUTPUT_VARIABLE build_output ERROR_VARIABLE build_errors TIMEOUT 600)
        hash_of("${build_output}" build_hash)
        if(NOT installed_hash OR NOT installed_hash STREQUAL build_hash)
            problem("installed dve_player hash '${installed_hash}' != build-tree hash '${build_hash}'")
        endif()
    endif()
    if(EXPECT AND installed_hash)
        string(REPLACE "," ";" expected_hashes "${EXPECT}")
        if(NOT installed_hash IN_LIST expected_hashes)
            problem("installed dve_player hash ${installed_hash} is not a golden hash (${EXPECT})")
        endif()
    endif()
    message(STATUS "installed dve_player framebuffer_fnv=${installed_hash}")
endif()

# The installed dve_pack works from the prefix too.
if(PROJECT AND EXISTS "${PREFIX}/${DVE_BINDIR}/dve_pack")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${PREFIX}/${DVE_BINDIR}/dve_pack"
            "${PROJECT}" "${WORK_DIR}/repacked.dvepak" --all
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0 OR NOT EXISTS "${WORK_DIR}/repacked.dvepak")
        problem("installed dve_pack failed (${result}):\n${output}\n${errors}")
    endif()
endif()

# The installed editor passes its smoke test (offscreen video, no audio device needed).
if(EXISTS "${PREFIX}/${DVE_BINDIR}/dve_desktop_editor")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH SDL_VIDEO_DRIVER=offscreen
            SDL_AUDIO_DRIVER=dummy "${PREFIX}/${DVE_BINDIR}/dve_desktop_editor" --smoke
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 300)
    if(NOT result EQUAL 0 OR NOT output MATCHES "dve_desktop_editor: PASS")
        problem("installed dve_desktop_editor --smoke failed (${result}):\n${output}\n${errors}")
    endif()
endif()

if(problems)
    list(JOIN problems "\n  " text)
    message(FATAL_ERROR "install tree problems:\n  ${text}")
endif()
message(STATUS "dve_install_tree_test: PASS")
