# Test coverage report: which test groups this configuration builds and which it leaves out.
#
# A build where whole groups of tests are switched off (Lua, Vulkan, Jolt, the DXC shader tests,
# ...) still says "100% tests passed", which hides that those tests never ran. This module:
#   - lists every optional test group with whether it is configured and, if not, why;
#   - prints the skipped groups as a warning at the end of configuration;
#   - writes the report to ${CMAKE_BINARY_DIR}/dve_test_coverage.txt;
#   - registers the test dve_test_coverage, which prints the report and is reported "Skipped"
#     by CTest when any group is missing, so the summary of every ctest run shows it;
#   - gives every test SKIP_RETURN_CODE 77 (tests/dve_test_skip.hpp), so a test that skipped
#     part of its checks at run time is "Skipped" rather than "Passed".
# DVE_REQUIRE_COMPLETE_TESTS=ON makes configuration fail when any group is missing, for CI that
# must run everything (set the environment variable of the same name for run-time skips).

option(DVE_REQUIRE_COMPLETE_TESTS "Fail configuration when an optional test group is not built" OFF)

set(_dve_coverage_lines "")
set(_dve_coverage_skipped "")

# dve_test_group(<name> CONDITION <if() arguments...> REASON <why it is missing>)
function(dve_test_group name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "REASON" "CONDITION")
    if(${ARG_CONDITION})
        set(_line "  built    ${name}")
    else()
        set(_line "  SKIPPED  ${name}: ${ARG_REASON}")
        set(_dve_coverage_skipped "${_dve_coverage_skipped};${name}" PARENT_SCOPE)
    endif()
    set(_dve_coverage_lines "${_dve_coverage_lines}${_line}\n" PARENT_SCOPE)
endfunction()

if(DVE_BUILD_TESTS)
    if(NOT DEFINED _dve_dxc_spirv)
        set(_dve_dxc_spirv OFF)
    endif()
    dve_test_group("Lua gameplay scripting (test_game_script, Lua parts of test_game_save)"
        CONDITION DVE_ENABLE_LUA REASON "DVE_ENABLE_LUA=OFF (or no Lua 5.4 found)")
    dve_test_group("Vulkan GPU tests (RHI, AO, atmosphere, material tables, live renderer)"
        CONDITION TARGET dve_rhi_vulkan REASON "Vulkan backend not built (DVE_BUILD_RHI / DVE_ENABLE_VULKAN_BUFFER_BACKEND)")
    dve_test_group("DXC-compiled GPU shader conformance (surface patterns, live IBL, clear glass)"
        CONDITION _dve_dxc_spirv REASON "no DXC with SPIR-V codegen found (set DVE_DXC_EXECUTABLE)")
    dve_test_group("Jolt physics backend (Jolt round trips, contact filters)"
        CONDITION DVE_ENABLE_JOLT REASON "DVE_ENABLE_JOLT=OFF")
    dve_test_group("Editor (material panel, layers, weathering, import)"
        CONDITION TARGET dve_editor REASON "DVE_BUILD_EDITOR=OFF")
    dve_test_group("Player runtime"
        CONDITION TARGET dve_player_runtime REASON "DVE_BUILD_PLAYER / DVE_BUILD_RHI / DVE_BUILD_PLATFORM off")
    dve_test_group("Asset pipeline (PNG / JPEG)"
        CONDITION TARGET dve_asset_pipeline REASON "DVE_BUILD_ASSET_PIPELINE_TESTS=OFF or no image codecs")
    dve_test_group("Mesh authoring and print export (Manifold)"
        CONDITION TARGET manifold REASON "DVE_ENABLE_MESH_AUTHORING / DVE_ENABLE_PRINT_EXPORT off or Manifold source missing")
    dve_test_group("Audio synthesis"
        CONDITION DVE_BUILD_AUDIO_SYNTH REASON "DVE_BUILD_AUDIO_SYNTH=OFF")
    dve_test_group("Box2D backend"
        CONDITION DVE_ENABLE_BOX2D REASON "DVE_ENABLE_BOX2D=OFF")
    dve_test_group("Box3D backend"
        CONDITION DVE_ENABLE_BOX3D REASON "DVE_ENABLE_BOX3D=OFF")

    set(_dve_coverage_report "DVE test coverage for this configuration:\n${_dve_coverage_lines}")
    list(REMOVE_ITEM _dve_coverage_skipped "")
    list(LENGTH _dve_coverage_skipped _dve_skipped_count)
    if(_dve_skipped_count GREATER 0)
        string(APPEND _dve_coverage_report
            "${_dve_skipped_count} test group(s) are not built: a passing ctest run does not cover them.\n")
    else()
        string(APPEND _dve_coverage_report "Every optional test group is built.\n")
    endif()
    file(WRITE "${CMAKE_BINARY_DIR}/dve_test_coverage.txt" "${_dve_coverage_report}")

    if(_dve_skipped_count GREATER 0)
        if(DVE_REQUIRE_COMPLETE_TESTS)
            message(FATAL_ERROR "${_dve_coverage_report}DVE_REQUIRE_COMPLETE_TESTS=ON")
        endif()
        message(WARNING "${_dve_coverage_report}")
    else()
        message(STATUS "DVE test coverage: every optional test group is built")
    endif()

    add_test(NAME dve_test_coverage
        COMMAND "${CMAKE_COMMAND}" -E cat "${CMAKE_BINARY_DIR}/dve_test_coverage.txt")
    set_tests_properties(dve_test_coverage PROPERTIES SKIP_REGULAR_EXPRESSION "  SKIPPED  ")

    # Run-time skips (tests/dve_test_skip.hpp) exit 77: "Skipped", not "Passed".
    get_property(_dve_coverage_tests DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY TESTS)
    foreach(_dve_test IN LISTS _dve_coverage_tests)
        get_test_property("${_dve_test}" SKIP_RETURN_CODE _dve_existing)
        if(NOT _dve_existing)
            set_tests_properties("${_dve_test}" PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()
