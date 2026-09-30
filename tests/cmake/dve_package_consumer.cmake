# Package-consumer test (packaging plan §6): configure, build and run tests/package_consumer (a
# tiny game using find_package(dve) + dve::player_runtime) against the prefix installed by
# dve_install_tree_test, then check the SameMinorVersion rule of dveConfigVersion.cmake.
#   PREFIX SOURCE WORK_DIR GENERATOR CXX_COMPILER BUILD_TYPE VERSION PAK [GAME_PROJECT]
cmake_minimum_required(VERSION 3.24)
foreach(required PREFIX SOURCE WORK_DIR GENERATOR CXX_COMPILER VERSION PAK)
    if(NOT ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()
if(NOT EXISTS "${PREFIX}/lib/cmake/dve/dveConfig.cmake" AND NOT EXISTS "${PREFIX}/lib64/cmake/dve/dveConfig.cmake")
    message(FATAL_ERROR "no dve package in ${PREFIX}; dve_install_tree_test must run first")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)" _ "${VERSION}")
set(major "${CMAKE_MATCH_1}")
set(minor "${CMAKE_MATCH_2}")
if(NOT BUILD_TYPE)
    set(BUILD_TYPE Release)
endif()

# Only the scratch prefix may satisfy find_package(dve): no registries, no system dve.
set(isolation
    -DCMAKE_PREFIX_PATH=${PREFIX}
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
    -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=ON)

function(configure name required out_result out_log)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${WORK_DIR}/${name}" -G "${GENERATOR}"
            -DCMAKE_CXX_COMPILER=${CXX_COMPILER} -DCMAKE_BUILD_TYPE=${BUILD_TYPE}
            -DDVE_REQUIRED_VERSION=${required} ${isolation} ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    set(${out_result} "${result}" PARENT_SCOPE)
    set(${out_log} "${output}\n${errors}" PARENT_SCOPE)
endfunction()

set(game_args "")
if(GAME_PROJECT)
    set(game_args -DDVE_CONSUMER_GAME_PROJECT=${GAME_PROJECT})
endif()
configure(game "${major}.${minor}" result log ${game_args})
if(NOT result EQUAL 0)
    message(FATAL_ERROR "configuring the consumer against ${PREFIX} failed:\n${log}")
endif()
message(STATUS "${log}")
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK_DIR}/game"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "building the consumer failed:\n${output}\n${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH "${WORK_DIR}/game/tiny_game" "${PAK}" 10
    WORKING_DIRECTORY "${WORK_DIR}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 300)
message(STATUS "${output}${errors}")
if(NOT result EQUAL 0 OR NOT output MATCHES "dve_package_consumer: PASS")
    message(FATAL_ERROR "tiny_game failed (${result})")
endif()
if(NOT output MATCHES "dve_version=${VERSION} ")
    message(FATAL_ERROR "tiny_game saw the wrong dve/version.hpp")
endif()

# dve_add_game_package() through the installed package.
if(GAME_PROJECT)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=LD_LIBRARY_PATH
            "${CMAKE_COMMAND}" --build "${WORK_DIR}/game" --target consumer_game_package
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0 OR NOT EXISTS "${WORK_DIR}/game/ConsumerGame/ConsumerGame"
            OR NOT output MATCHES "verified: ConsumerGame")
        message(FATAL_ERROR "dve_add_game_package from the installed package failed (${result}):\n${output}\n${errors}")
    endif()
    message(STATUS "dve_add_game_package (installed): OK")
endif()

# SameMinorVersion: the exact version and X.Y are accepted; another minor version is not.
configure(exact "${VERSION}" result log)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "find_package(dve ${VERSION}) failed:\n${log}")
endif()
math(EXPR next_minor "${minor} + 1")
math(EXPR previous_minor "${minor} - 1")
foreach(incompatible "${major}.${next_minor}" "${major}.${previous_minor}")
    configure("reject_${incompatible}" "${incompatible}" result log)
    if(result EQUAL 0)
        message(FATAL_ERROR "find_package(dve ${incompatible}) accepted dve ${VERSION} (expected SameMinorVersion to reject it)")
    endif()
endforeach()
message(STATUS "dve_package_consumer_test: PASS")
