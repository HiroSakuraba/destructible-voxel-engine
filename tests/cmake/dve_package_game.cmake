# End-to-end game packaging test (packaging Phase 4, plan §6 dve_package_game_test).
#
# Uses the installed prefix of the dve_install_prefix fixture (Runtime, RuntimeDeps and Tools
# as `cmake --install` put them), so it exercises exactly what a user of the packages gets:
#   A. tests/data/player_sample -> dve_package_game --tgz --verify; the archive is extracted to
#      a scratch folder and the shipped executable runs from an unrelated working directory with
#      nothing on LD_LIBRARY_PATH; its framebuffer hash must equal the build-tree dve_player's
#      on the same pak (and a golden hash when one is known).
#   B. a copy of examples/editor_demo_project (editor scene + .autosave/) with a game.dvegame
#      naming the exported scene: dve_package_game must export it with dve_export_scene and
#      keep .autosave/, the .dvescene and its revision folder out of the pak.
# Both: the RPATH, ldd resolution inside the folder, THIRD_PARTY_NOTICES covering every
# shipped .so (generate_third_party_notices.py --verify-dir), build-info.json, no sample maps.
#   PREFIX PYTHON SCRIPT NOTICES_TOOL SAMPLE EDITOR_PROJECT WORK_DIR BUILD_PLAYER SETTINGS [EXPECT]
cmake_minimum_required(VERSION 3.24)
foreach(required PREFIX PYTHON SCRIPT NOTICES_TOOL SAMPLE EDITOR_PROJECT WORK_DIR BUILD_PLAYER SETTINGS)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
include("${SETTINGS}")
set(SYSTEM_EXCLUDES ${DVE_SYSTEM_EXCLUDES})
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/elsewhere")
set(problems "")
macro(problem text)
    list(APPEND problems "${text}")
endmacro()
set(ENV_ARGS --unset=LD_LIBRARY_PATH --unset=LD_PRELOAD SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)
set(hash_args --frames 30 --fixed-dt 0.016666668 --render-size 480x270 --threads 4 --size 960x540 --hash)

function(package_game project output out_log)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${PYTHON}" "${SCRIPT}"
            --project "${project}" --output "${output}" --runtime-prefix "${PREFIX}" ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output_text ERROR_VARIABLE error_text TIMEOUT 900)
    message(STATUS "dve_package_game ${project}:\n${output_text}${error_text}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "dve_package_game failed (${result})")
    endif()
    set(${out_log} "${output_text}${error_text}" PARENT_SCOPE)
endfunction()

function(check_folder folder exe)
    if(NOT EXISTS "${folder}/${exe}")
        problem("${folder}/${exe} missing")
        set(problems "${problems}" PARENT_SCOPE)
        return()
    endif()
    foreach(file game.dvepak THIRD_PARTY_NOTICES.txt build-info.json)
        if(NOT EXISTS "${folder}/${file}")
            problem("${folder}/${file} missing")
        endif()
    endforeach()
    execute_process(COMMAND readelf -d "${folder}/${exe}" OUTPUT_VARIABLE dynamic)
    if(NOT dynamic MATCHES "\\(RPATH\\)[^\n]*\\$ORIGIN/lib/dve")
        problem("${exe}: no DT_RPATH with $ORIGIN/lib/dve")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} ldd "${folder}/${exe}" OUTPUT_VARIABLE resolved)
    string(REPLACE "\n" ";" lines "${resolved}")
    foreach(line IN LISTS lines)
        if(line MATCHES "not found")
            problem("${exe}: unresolved dependency: ${line}")
        elseif(line MATCHES "^[ \t]*([^ \t]+) => ([^ \t]+)")
            set(name "${CMAKE_MATCH_1}")
            set(path "${CMAKE_MATCH_2}")
            string(FIND "${path}" "${folder}/" inside)
            if(inside EQUAL 0)
                continue()
            endif()
            set(system OFF)
            foreach(pattern IN LISTS SYSTEM_EXCLUDES)
                if(name MATCHES "${pattern}")
                    set(system ON)
                endif()
            endforeach()
            if(NOT system)
                problem("${exe}: ${name} resolves outside the game folder (${path})")
            endif()
        endif()
    endforeach()
    execute_process(COMMAND "${PYTHON}" "${NOTICES_TOOL}" --verify-dir "${folder}/lib/dve"
            --notices "${folder}/THIRD_PARTY_NOTICES.txt"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        problem("THIRD_PARTY_NOTICES does not cover ${folder}/lib/dve:\n${out}${err}")
    endif()
    file(GLOB_RECURSE everything RELATIVE "${folder}" "${folder}/*")
    foreach(path IN LISTS everything)
        if(path MATCHES "sample_maps|\\.autosave|\\.dvescene$")
            problem("editor-only or sample-map file shipped: ${path}")
        endif()
    endforeach()
    file(STRINGS "${folder}/game.dvepak" leaked REGEX "\\.autosave/|\\.dvescene|\\.objects\\.r[0-9]|project\\.dveproject")
    if(leaked)
        problem("${folder}/game.dvepak contains editor-only entries: ${leaked}")
    endif()
    set(problems "${problems}" PARENT_SCOPE)
endfunction()

function(folder_size folder out)
    file(GLOB_RECURSE files LIST_DIRECTORIES false "${folder}/*")
    set(total 0)
    foreach(file IN LISTS files)
        if(NOT IS_SYMLINK "${file}")
            file(SIZE "${file}" size)
            math(EXPR total "${total} + ${size}")
        endif()
    endforeach()
    math(EXPR kib "${total} / 1024")
    set(${out} "${kib}" PARENT_SCOPE)
endfunction()

# ---- A: the sample game, archived, extracted and run from elsewhere -----------------------
package_game("${SAMPLE}" "${WORK_DIR}/out/Player_Sample" log_a --tgz --verify)
if(NOT log_a MATCHES "verified: Player_Sample --frames 2")
    problem("dve_package_game --verify did not report a verified run")
endif()
file(GLOB archives "${WORK_DIR}/out/Player_Sample-*-linux-x86_64.tar.gz")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR "expected one Player_Sample tgz, found: ${archives}")
endif()
file(MAKE_DIRECTORY "${WORK_DIR}/extracted")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archives}" WORKING_DIRECTORY "${WORK_DIR}/extracted"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "could not extract ${archives}")
endif()
set(game "${WORK_DIR}/extracted/Player_Sample")
check_folder("${game}" Player_Sample)
file(SIZE "${archives}" archive_bytes)
math(EXPR archive_kib "${archive_bytes} / 1024")
folder_size("${game}" game_kib)
file(GLOB game_libs RELATIVE "${game}/lib/dve" "${game}/lib/dve/*")
message(STATUS "Player_Sample: folder ${game_kib} KiB, tgz ${archive_kib} KiB, lib/dve: ${game_libs}")

# Default content discovery: <exe dir>/game.dvepak, run from an unrelated working directory.
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} "${game}/Player_Sample" ${hash_args}
    WORKING_DIRECTORY "${WORK_DIR}/elsewhere"
    RESULT_VARIABLE result OUTPUT_VARIABLE shipped_output ERROR_VARIABLE shipped_errors TIMEOUT 600)
if(NOT result EQUAL 0 OR NOT shipped_output MATCHES "dve_player: PASS")
    problem("shipped Player_Sample failed (${result}):\n${shipped_output}\n${shipped_errors}")
endif()
if(NOT shipped_output MATCHES "content=pak:[^\n]*Player_Sample/game\\.dvepak")
    problem("shipped Player_Sample did not find its game.dvepak next to the executable:\n${shipped_output}")
endif()
if(NOT shipped_output MATCHES "objects=4\n")
    problem("shipped Player_Sample did not load the 4 sample objects")
endif()
string(REGEX MATCH "framebuffer_fnv=([0-9a-f]+)" _ "${shipped_output}")
set(shipped_hash "${CMAKE_MATCH_1}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} "${BUILD_PLAYER}" --pak "${game}/game.dvepak" ${hash_args}
    WORKING_DIRECTORY "${WORK_DIR}/elsewhere"
    RESULT_VARIABLE result OUTPUT_VARIABLE build_output ERROR_VARIABLE build_errors TIMEOUT 600)
string(REGEX MATCH "framebuffer_fnv=([0-9a-f]+)" _ "${build_output}")
set(build_hash "${CMAKE_MATCH_1}")
if(NOT shipped_hash OR NOT shipped_hash STREQUAL build_hash)
    problem("shipped game hash '${shipped_hash}' != build-tree dve_player hash '${build_hash}'")
endif()
if(EXPECT AND shipped_hash)
    string(REPLACE "," ";" expected "${EXPECT}")
    if(NOT shipped_hash IN_LIST expected)
        problem("shipped game hash ${shipped_hash} is not a golden hash (${EXPECT})")
    endif()
endif()
message(STATUS "shipped Player_Sample framebuffer_fnv=${shipped_hash}")
file(READ "${game}/build-info.json" info)
if(NOT info MATCHES "\"packageHash\": \"[0-9a-fx]+\"" OR NOT info MATCHES "\"name\": \"Player Sample\"")
    problem("build-info.json is incomplete:\n${info}")
endif()

# Re-packaging into the same folder is allowed (it is recognised by build-info.json) and a
# non-empty foreign folder is refused.
file(MAKE_DIRECTORY "${WORK_DIR}/foreign")
file(WRITE "${WORK_DIR}/foreign/keep.txt" "not ours")
execute_process(COMMAND "${PYTHON}" "${SCRIPT}" --project "${SAMPLE}" --output "${WORK_DIR}/foreign"
        --runtime-prefix "${PREFIX}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
if(result EQUAL 0 OR NOT EXISTS "${WORK_DIR}/foreign/keep.txt")
    problem("dve_package_game replaced a folder it did not create")
endif()

# ---- B: an editor project (scene export + .autosave exclusion) ----------------------------
set(editor_copy "${WORK_DIR}/editor_project")
file(COPY "${EDITOR_PROJECT}/" DESTINATION "${editor_copy}")
if(NOT EXISTS "${editor_copy}/.autosave")
    problem("test setup: the editor demo project copy has no .autosave folder")
endif()
file(WRITE "${editor_copy}/game.dvegame"
    "DVE_GAME 1\nname=Editor Demo\nversion=1.0.0\nentryScene=scenes/editor_demo.dvoxscene.json\ncamera=3,3,6 -> 0,0.5,0\n")
package_game("${editor_copy}" "${WORK_DIR}/out/EditorDemo" log_b --name EditorDemo --verify)
if(NOT log_b MATCHES "exported 1 editor scene")
    problem("dve_package_game did not export the editor scene:\n${log_b}")
endif()
set(editor_game "${WORK_DIR}/out/EditorDemo")
check_folder("${editor_game}" EditorDemo)
file(READ "${editor_game}/build-info.json" info_b)
if(NOT info_b MATCHES "\"scenes/editor_demo.dvoxscene.json\"" OR NOT info_b MATCHES "\\.autosave/")
    problem("build-info.json does not record the exported scene and the excluded .autosave/:\n${info_b}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} "${editor_game}/EditorDemo" --frames 5 --hash
        --size 320x180 --render-size 320x180 --threads 2
    WORKING_DIRECTORY "${WORK_DIR}/elsewhere"
    RESULT_VARIABLE result OUTPUT_VARIABLE editor_output ERROR_VARIABLE editor_errors TIMEOUT 600)
if(NOT result EQUAL 0 OR NOT editor_output MATCHES "objects=1\n" OR NOT editor_output MATCHES "dve_player: PASS")
    problem("packaged editor scene did not run (${result}):\n${editor_output}\n${editor_errors}")
endif()
folder_size("${editor_game}" editor_kib)
message(STATUS "EditorDemo: folder ${editor_kib} KiB")

if(problems)
    list(JOIN problems "\n  " text)
    message(FATAL_ERROR "game packaging problems:\n  ${text}")
endif()
message(STATUS "dve_package_game_test: PASS (Player_Sample ${game_kib} KiB folder, ${archive_kib} KiB tgz)")
