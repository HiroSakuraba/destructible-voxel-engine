# Windows game packaging test (dve_package_game_windows_test).
#
# The Linux dve_package_game_test works from the installed prefix and checks RPATH/ldd; on
# Windows the same end-to-end path is checked with Windows tools:
#   tests/data/player_sample -> dve_package_game.py --build-dir <build> --config <cfg> --zip
#   --verify (Runtime + RuntimeDeps installed by cmake --install into a scratch prefix).
#   The zip is extracted to a scratch folder and <Game>.exe runs from an unrelated working
#   directory with PATH reduced to the Windows folders. Checks: the files of the folder,
#   DVE-LICENSE.txt == the installed engine license, build-info.json, every DLL the .exe and
#   its DLLs import is in the folder or a Windows system DLL and listed in
#   THIRD_PARTY_NOTICES.txt (generate_third_party_notices.py --verify-dir), no mpg123/mp3lame
#   DLL by file name or import, objects=4, and the framebuffer hash equals the build-tree
#   dve_player's on the same pak.
#   PYTHON SCRIPT NOTICES_TOOL MANIFEST SAMPLE WORK_DIR BUILD_DIR CONFIG BUILD_PLAYER CMAKE_EXE
cmake_minimum_required(VERSION 3.24)
foreach(required PYTHON SCRIPT NOTICES_TOOL MANIFEST SAMPLE WORK_DIR BUILD_DIR BUILD_PLAYER CMAKE_EXE)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/elsewhere" "${WORK_DIR}/extracted")
set(problems "")
# A function, not a macro: the messages contain Windows paths, whose backslashes a macro would
# re-parse as escape sequences.
function(problem text)
    list(APPEND problems "${text}")
    set(problems "${problems}" PARENT_SCOPE)
endfunction()
set(config_args "")
if(CONFIG)
    set(config_args --config "${CONFIG}")
endif()

execute_process(COMMAND "${PYTHON}" "${SCRIPT}" --project "${SAMPLE}" --output "${WORK_DIR}/out/Player_Sample"
        --build-dir "${BUILD_DIR}" --cmake "${CMAKE_EXE}" ${config_args} --zip --verify
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE err TIMEOUT 1200)
message(STATUS "dve_package_game:\n${log}${err}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "dve_package_game failed (${result})")
endif()
if(NOT "${log}${err}" MATCHES "verified: Player_Sample --frames 2")
    problem("dve_package_game --verify did not report a verified run")
endif()
file(GLOB archives "${WORK_DIR}/out/Player_Sample-*-windows-x86_64.zip")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR "expected one Player_Sample zip, found: '${archives}'")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${archives}" WORKING_DIRECTORY "${WORK_DIR}/extracted"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "could not extract ${archives}")
endif()
set(game "${WORK_DIR}/extracted/Player_Sample")
foreach(file Player_Sample.exe game.dvepak THIRD_PARTY_NOTICES.txt DVE-LICENSE.txt build-info.json)
    if(NOT EXISTS "${game}/${file}")
        problem("${game}/${file} missing")
    endif()
endforeach()
if(EXISTS "${game}/DVE-LICENSE.txt")
    file(READ "${game}/DVE-LICENSE.txt" shipped_license)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../../LICENSE" source_license)
    if(NOT shipped_license STREQUAL source_license OR NOT shipped_license MATCHES "^MIT License")
        problem("DVE-LICENSE.txt is not the engine's MIT license")
    endif()
endif()
if(EXISTS "${game}/THIRD_PARTY_NOTICES.txt")
    file(READ "${game}/THIRD_PARTY_NOTICES.txt" shipped_notices)
    if(NOT shipped_notices MATCHES "is licensed under the MIT License" OR shipped_notices MATCHES "NO LICENSE")
        problem("THIRD_PARTY_NOTICES.txt does not state the engine's MIT license")
    endif()
    if(shipped_notices MATCHES "\nBundled file: (lib)?(mpg123|mp3lame)")
        problem("THIRD_PARTY_NOTICES.txt lists bundled MP3 libraries")
    endif()
endif()
if(EXISTS "${game}/build-info.json")
    file(READ "${game}/build-info.json" info)
    if(NOT info MATCHES "\"executable\": \"Player_Sample.exe\"" OR NOT info MATCHES "\"DVE-LICENSE.txt\"")
        problem("build-info.json does not record Player_Sample.exe / DVE-LICENSE.txt")
    endif()
    string(TOLOWER "${info}" info_lower)
    if(info_lower MATCHES "mpg123|mp3lame")
        problem("build-info.json records MP3 libraries")
    endif()
endif()
file(GLOB_RECURSE everything RELATIVE "${game}" "${game}/*")
foreach(path IN LISTS everything)
    string(TOLOWER "${path}" lower)
    if(lower MATCHES "mpg123|mp3lame")
        problem("MP3 library shipped: ${path}")
    endif()
    if(path MATCHES "sample_maps|\\.autosave|\\.dvescene$|\\.pdb$|\\.lib$")
        problem("file that should not ship: ${path}")
    endif()
endforeach()
execute_process(COMMAND "${PYTHON}" "${NOTICES_TOOL}" --verify-dir "${game}"
        --notices "${game}/THIRD_PARTY_NOTICES.txt" --manifest "${MANIFEST}"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT result EQUAL 0)
    problem("generate_third_party_notices.py --verify-dir ${game} failed:\n${out}${err}")
endif()

# Run from elsewhere with only the Windows folders on PATH.
set(system_root "$ENV{SystemRoot}")
if(NOT system_root)
    set(system_root "C:/Windows")
endif()
file(TO_NATIVE_PATH "${system_root}" system_root)
set(clean_path "${system_root}\\System32;${system_root};${system_root}\\System32\\Wbem")
# PATH goes through ENV{PATH} (a ';' in a "PATH=..." argument would split the CMake list).
set(saved_path "$ENV{PATH}")
set(ENV_ARGS SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy)
set(hash_args --frames 30 --fixed-dt 0.016666668 --render-size 480x270 --threads 4 --size 960x540 --hash --no-audio)
set(ENV{PATH} "${clean_path}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${ENV_ARGS} "${game}/Player_Sample.exe" ${hash_args}
    WORKING_DIRECTORY "${WORK_DIR}/elsewhere"
    RESULT_VARIABLE result OUTPUT_VARIABLE shipped_output ERROR_VARIABLE shipped_errors TIMEOUT 600)
set(ENV{PATH} "${saved_path}")
if(NOT result EQUAL 0 OR NOT shipped_output MATCHES "dve_player: PASS")
    problem("shipped Player_Sample.exe failed (${result}):\n${shipped_output}\n${shipped_errors}")
endif()
if(NOT shipped_output MATCHES "content=pak:[^\n]*Player_Sample[/\\\\]game\\.dvepak")
    problem("shipped Player_Sample.exe did not find its game.dvepak next to the executable:\n${shipped_output}")
endif()
if(NOT shipped_output MATCHES "objects=4\r?\n")
    problem("shipped Player_Sample.exe did not load the 4 sample objects:\n${shipped_output}")
endif()
string(REGEX MATCH "framebuffer_fnv=([0-9a-f]+)" _ "${shipped_output}")
set(shipped_hash "${CMAKE_MATCH_1}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy
        "${BUILD_PLAYER}" --pak "${game}/game.dvepak" ${hash_args}
    WORKING_DIRECTORY "${WORK_DIR}/elsewhere"
    RESULT_VARIABLE result OUTPUT_VARIABLE build_output ERROR_VARIABLE build_errors TIMEOUT 600)
string(REGEX MATCH "framebuffer_fnv=([0-9a-f]+)" _ "${build_output}")
set(build_hash "${CMAKE_MATCH_1}")
if(NOT shipped_hash OR NOT shipped_hash STREQUAL build_hash)
    problem("shipped game hash '${shipped_hash}' != build-tree dve_player hash '${build_hash}'\n${build_output}${build_errors}")
endif()
file(SIZE "${archives}" archive_bytes)
math(EXPR archive_kib "${archive_bytes} / 1024")
file(GLOB shipped RELATIVE "${game}" "${game}/*")
message(STATUS "Player_Sample zip: ${archive_kib} KiB; files: ${shipped}; framebuffer_fnv=${shipped_hash}")
if(problems)
    list(JOIN problems "\n  - " text)
    message(FATAL_ERROR "Windows game package problems:\n  - ${text}")
endif()
message(STATUS "dve_package_game_windows_test: PASS")
