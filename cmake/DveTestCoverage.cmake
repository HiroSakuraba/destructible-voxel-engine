# Test coverage report: which test groups this configuration builds and which it leaves out.
#
# A build where whole groups of tests are switched off (Lua, Vulkan, Jolt, the DXC shader tests,
# ...) still says "100% tests passed", which hides that those tests never ran. This module:
#   - lists every optional test group (by a short id) with whether it is configured and, if not, why;
#   - prints the skipped groups as a warning at the end of configuration;
#   - writes the report to ${CMAKE_BINARY_DIR}/dve_test_coverage.txt;
#   - registers the test dve_test_coverage, which prints the report and is reported "Skipped"
#     by CTest when any group is missing, so the summary of every ctest run shows it;
#   - gives every test that has no SKIP_RETURN_CODE of its own the code 77 (tests/dve_test_skip.hpp),
#     so a test that skipped part of its checks at run time is "Skipped" rather than "Passed".
#
# Which groups a job MUST have is the job's decision, because no job has all of them (hosted CI
# runners have no GPU and no DXC; the plain Release job has no Lua or Jolt; ...):
#   DVE_REQUIRE_TEST_GROUPS="Lua,Jolt"   (CMake cache variable, or the environment variable of the
#                                         same name when the cache variable is empty)
#       Configuration fails when one of the named groups is SKIPPED. Separators: , ; or spaces.
#       An unknown id fails too (typo guard), and so does naming groups with DVE_BUILD_TESTS=OFF.
#   DVE_REQUIRE_COMPLETE_TESTS=ON
#       Every group is required (a job that can run everything). The environment variable of the
#       same name makes a run-time skip (dve_test_skip.hpp) a failure of the test.
#
# Everything is evaluated by a call deferred to the end of the top-level CMakeLists.txt, so the
# targets and tests created after this file is included (region tests, packaging tests, ...) are
# seen, in subdirectories too.

option(DVE_REQUIRE_COMPLETE_TESTS "Fail configuration when an optional test group is not built" OFF)
set(DVE_REQUIRE_TEST_GROUPS "" CACHE STRING
    "Test groups this build must have (ids from dve_test_coverage.txt, separated by , or ;)")

# _dve_test_group(<id> DESCRIPTION <text> REASON <why it can be missing> CONDITION <if() arguments...>)
# Appends to _dve_coverage_report / _dve_coverage_skipped / _dve_coverage_ids in the caller.
# Ids are plain words, so the lists below never contain a ";" inside an element (descriptions and
# reasons are only ever put into strings, never into lists).
function(_dve_test_group id)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "DESCRIPTION;REASON" "CONDITION")
    if(NOT id MATCHES "^[A-Za-z0-9_]+$" OR NOT DEFINED ARG_CONDITION)
        message(FATAL_ERROR "DveTestCoverage: bad test group '${id}' (id must be [A-Za-z0-9_]+, CONDITION is required)")
    endif()
    # CONDITION is a list of if() arguments such as "TARGET dve_editor" or "DVE_ENABLE_LUA".
    if(${ARG_CONDITION})
        set(_line "  built    ${id}: ${ARG_DESCRIPTION}")
    else()
        set(_line "  SKIPPED  ${id}: ${ARG_DESCRIPTION} -- ${ARG_REASON}")
        set(_dve_coverage_skipped "${_dve_coverage_skipped};${id}" PARENT_SCOPE)
    endif()
    set(_dve_coverage_ids "${_dve_coverage_ids};${id}" PARENT_SCOPE)
    set(_dve_coverage_lines "${_dve_coverage_lines}${_line}\n" PARENT_SCOPE)
endfunction()

# Gives every test of <dir> and of its subdirectories that has no SKIP_RETURN_CODE the code 77.
# Tests of subdirectories can only be addressed with the DIRECTORY signature (CMake 3.28+).
function(_dve_apply_skip_return_code dir root_dir)
    get_property(_tests DIRECTORY "${dir}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
            get_test_property("${_test}" SKIP_RETURN_CODE DIRECTORY "${dir}" _existing)
            if(_existing STREQUAL "NOTFOUND")
                set_property(TEST "${_test}" DIRECTORY "${dir}" PROPERTY SKIP_RETURN_CODE 77)
            endif()
        elseif(dir STREQUAL root_dir)
            get_test_property("${_test}" SKIP_RETURN_CODE _existing)
            if(_existing STREQUAL "NOTFOUND")
                set_tests_properties("${_test}" PROPERTIES SKIP_RETURN_CODE 77)
            endif()
        else()
            message(WARNING "DveTestCoverage: test ${_test} in ${dir} cannot get SKIP_RETURN_CODE (needs CMake 3.28)")
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        # Vendored and fetched dependencies (Jolt, SDL, Manifold, ...) keep their own tests as they are.
        if(_sub MATCHES "/(third_party|_deps|_deps/[^/]*)(/|$)" OR NOT _sub MATCHES "^${root_dir}/")
            continue()
        endif()
        _dve_apply_skip_return_code("${_sub}" "${root_dir}")
    endforeach()
endfunction()

function(_dve_finish_test_coverage root_dir)
    # Which groups this job requires.
    set(_required "${DVE_REQUIRE_TEST_GROUPS}")
    set(_required_from "-DDVE_REQUIRE_TEST_GROUPS")
    if(_required STREQUAL "" AND NOT "$ENV{DVE_REQUIRE_TEST_GROUPS}" STREQUAL "")
        set(_required "$ENV{DVE_REQUIRE_TEST_GROUPS}")
        set(_required_from "environment variable DVE_REQUIRE_TEST_GROUPS")
    endif()
    string(REGEX REPLACE "[,; \t\r\n]+" ";" _required "${_required}")
    list(REMOVE_ITEM _required "")
    list(REMOVE_DUPLICATES _required)

    if(NOT DVE_BUILD_TESTS)
        if(_required OR DVE_REQUIRE_COMPLETE_TESTS)
            message(FATAL_ERROR "Test groups are required (${_required_from}=\"${_required}\", "
                "DVE_REQUIRE_COMPLETE_TESTS=${DVE_REQUIRE_COMPLETE_TESTS}) but DVE_BUILD_TESTS=OFF")
        endif()
        return()
    endif()

    if(NOT DEFINED _dve_dxc_spirv)
        set(_dve_dxc_spirv OFF)
    endif()
    set(_dve_coverage_lines "")
    set(_dve_coverage_skipped "")
    set(_dve_coverage_ids "")
    _dve_test_group(Lua DESCRIPTION "Lua gameplay scripting (test_game_script, Lua parts of test_game_save)"
        CONDITION DVE_ENABLE_LUA REASON "DVE_ENABLE_LUA=OFF")
    _dve_test_group(Vulkan DESCRIPTION "Vulkan GPU tests (RHI, AO, atmosphere, material tables, live renderer), which still skip at run time without a device"
        CONDITION TARGET dve_rhi_vulkan REASON "Vulkan backend not built (DVE_BUILD_RHI / DVE_ENABLE_VULKAN_BUFFER_BACKEND)")
    _dve_test_group(DXC DESCRIPTION "DXC-compiled GPU shader conformance (surface patterns, live IBL, clear glass)"
        CONDITION _dve_dxc_spirv REASON "no DXC with SPIR-V codegen found (set DVE_DXC_EXECUTABLE)")
    _dve_test_group(Jolt DESCRIPTION "Jolt physics backend (Jolt round trips, contact filters)"
        CONDITION DVE_ENABLE_JOLT REASON "DVE_ENABLE_JOLT=OFF")
    _dve_test_group(Editor DESCRIPTION "Editor (material panel, layers, weathering, import)"
        CONDITION TARGET dve_editor REASON "DVE_BUILD_EDITOR=OFF")
    _dve_test_group(Player DESCRIPTION "Player runtime"
        CONDITION TARGET dve_player_runtime REASON "DVE_BUILD_PLAYER / DVE_BUILD_RHI / DVE_BUILD_PLATFORM off")
    _dve_test_group(AssetPipeline DESCRIPTION "Asset pipeline (PNG / JPEG)"
        CONDITION DVE_BUILD_ASSET_PIPELINE_TESTS AND TARGET dve_asset_pipeline REASON "DVE_BUILD_ASSET_PIPELINE_TESTS=OFF or no image codecs")
    _dve_test_group(MeshAuthoring DESCRIPTION "Mesh authoring and print export (Manifold)"
        CONDITION TARGET manifold REASON "DVE_ENABLE_MESH_AUTHORING / DVE_ENABLE_PRINT_EXPORT off or Manifold source missing")
    _dve_test_group(AudioSynth DESCRIPTION "Audio synthesis"
        CONDITION DVE_BUILD_AUDIO_SYNTH REASON "DVE_BUILD_AUDIO_SYNTH=OFF")
    _dve_test_group(Box2D DESCRIPTION "Box2D backend"
        CONDITION DVE_ENABLE_BOX2D REASON "DVE_ENABLE_BOX2D=OFF")
    _dve_test_group(Box3D DESCRIPTION "Box3D backend"
        CONDITION DVE_ENABLE_BOX3D REASON "DVE_ENABLE_BOX3D=OFF")
    list(REMOVE_ITEM _dve_coverage_skipped "")
    list(REMOVE_ITEM _dve_coverage_ids "")

    # A group that is required but unknown, or SKIPPED, is a configuration error.
    if(DVE_REQUIRE_COMPLETE_TESTS)
        set(_required ${_dve_coverage_ids})
        set(_required_from "DVE_REQUIRE_COMPLETE_TESTS=ON")
    endif()
    set(_unknown "")
    set(_missing "")
    foreach(_group IN LISTS _required)
        if(NOT _group IN_LIST _dve_coverage_ids)
            list(APPEND _unknown "${_group}")
        elseif(_group IN_LIST _dve_coverage_skipped)
            list(APPEND _missing "${_group}")
        endif()
    endforeach()

    list(LENGTH _dve_coverage_skipped _dve_skipped_count)
    string(REPLACE ";" ", " _required_text "${_required}")
    string(REPLACE ";" ", " _skipped_text "${_dve_coverage_skipped}")
    set(_report "DVE test coverage for this configuration:\n${_dve_coverage_lines}")
    if(_required)
        string(APPEND _report "Required by this build (${_required_from}): ${_required_text}\n")
    endif()
    if(_dve_skipped_count GREATER 0)
        string(APPEND _report
            "${_dve_skipped_count} test group(s) are not built (${_skipped_text}): a passing ctest run does not cover them.\n")
    else()
        string(APPEND _report "Every optional test group is built.\n")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/dve_test_coverage.txt" "${_report}")

    if(_unknown)
        string(REPLACE ";" ", " _unknown_text "${_unknown}")
        string(REPLACE ";" ", " _known_text "${_dve_coverage_ids}")
        message(FATAL_ERROR "${_required_from} names unknown test group(s): ${_unknown_text}. Known groups: ${_known_text}")
    endif()
    if(_missing)
        string(REPLACE ";" ", " _missing_text "${_missing}")
        message(FATAL_ERROR "${_report}\nRequired test group(s) not built: ${_missing_text} (required by ${_required_from}). "
            "A runner or option change made them silently disappear from this job's ctest run.")
    endif()
    if(_dve_skipped_count GREATER 0)
        message(WARNING "${_report}")
    else()
        message(STATUS "DVE test coverage: every optional test group is built")
    endif()

    add_test(NAME dve_test_coverage
        COMMAND "${CMAKE_COMMAND}" -E cat "${CMAKE_BINARY_DIR}/dve_test_coverage.txt")
    set_tests_properties(dve_test_coverage PROPERTIES SKIP_REGULAR_EXPRESSION "  SKIPPED  ")

    # Run-time skips (tests/dve_test_skip.hpp) exit 77: "Skipped", not "Passed". Tests that
    # already have a SKIP_RETURN_CODE (live editor MCP) keep theirs.
    _dve_apply_skip_return_code("${root_dir}" "${root_dir}")
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL _dve_finish_test_coverage "${CMAKE_CURRENT_SOURCE_DIR}")
