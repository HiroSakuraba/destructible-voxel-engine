cmake_minimum_required(VERSION 3.24)
# The checked-in sample .dvox files must match what the generator produces.
file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(COMMAND "${GENERATOR}" "${WORK_DIR}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "generator failed (${result}): ${output}${errors}")
endif()
file(GLOB generated RELATIVE "${WORK_DIR}/scenes" "${WORK_DIR}/scenes/*.dvox")
if(NOT generated)
    message(FATAL_ERROR "generator wrote no assets")
endif()
foreach(asset IN LISTS generated)
    execute_process(COMMAND ${CMAKE_COMMAND} -E compare_files
        "${WORK_DIR}/scenes/${asset}" "${SAMPLE_DIR}/scenes/${asset}" RESULT_VARIABLE different)
    if(different)
        message(FATAL_ERROR "tests/data/player_sample/scenes/${asset} is stale; rerun "
                            "dve_player_sample_generator tests/data/player_sample")
    endif()
endforeach()
