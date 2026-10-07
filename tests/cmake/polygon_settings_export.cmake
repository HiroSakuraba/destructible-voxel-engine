cmake_minimum_required(VERSION 3.24)
file(REMOVE_RECURSE "${WORK_DIR}")
function(run)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "command failed (${result}): ${output}\n${errors}")
    endif()
endfunction()
run("${HELPER}" --export-fixture "${WORK_DIR}")
execute_process(COMMAND "${EXPORTER}" "${WORK_DIR}/scene.dvescene" "${WORK_DIR}/same-output.txt"
    --project-root "${WORK_DIR}" --game-settings-output "${WORK_DIR}/same-output.txt"
    RESULT_VARIABLE rejected OUTPUT_QUIET ERROR_QUIET TIMEOUT 30)
if(NOT rejected EQUAL 2 OR EXISTS "${WORK_DIR}/same-output.txt")
    message(FATAL_ERROR "identical output files were not rejected before writing")
endif()
run("${EXPORTER}" "${WORK_DIR}/scene.dvescene" "${WORK_DIR}/scene.dvoxscene.json"
    --project-root "${WORK_DIR}" --game-settings-output "${WORK_DIR}/game_settings.txt" --quiet)
# Repeat export to verify safe replacement of an existing settings file on Windows.
run("${EXPORTER}" "${WORK_DIR}/scene.dvescene" "${WORK_DIR}/scene.dvoxscene.json"
    --project-root "${WORK_DIR}" --game-settings-output "${WORK_DIR}/game_settings.txt" --quiet)
run("${HELPER}" --verify-export "${WORK_DIR}")
file(REMOVE_RECURSE "${WORK_DIR}")
