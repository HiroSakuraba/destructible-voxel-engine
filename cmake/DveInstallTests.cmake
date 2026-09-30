# Tests for the install tree, the exported package and CPack (packaging plan §6).
include_guard(GLOBAL)
if(NOT DVE_BUILD_TESTS)
    return()
endif()

add_test(NAME dve_version_consistency
    COMMAND ${CMAKE_COMMAND}
        -DVERSION=${PROJECT_VERSION}
        -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DVERSION_HEADER=${DVE_GENERATED_INCLUDE_DIR}/dve/version.hpp
        -P ${PROJECT_SOURCE_DIR}/tests/cmake/dve_version_consistency.cmake)

# THIRD_PARTY_NOTICES (Phase 4): the generator's own tests, and a strict check per component
# that every bundled shared library, third-party target and archive has a manifest entry with a
# readable license text.
if(DVE_PYTHON3_EXECUTABLE)
    add_test(NAME dve_third_party_notices_self_test
        COMMAND ${DVE_PYTHON3_EXECUTABLE} ${DVE_NOTICES_GENERATOR} --self-test)
    foreach(_dve_component IN LISTS DVE_NOTICES_COMPONENTS)
        add_test(NAME dve_third_party_notices_check_${_dve_component}
            COMMAND ${DVE_PYTHON3_EXECUTABLE} ${DVE_NOTICES_GENERATOR} --check
                --manifest ${DVE_NOTICES_MANIFEST} --inputs ${DVE_NOTICES_INPUTS_${_dve_component}})
    endforeach()
endif()

if(NOT DVE_INSTALLED_COMPONENTS OR NOT UNIX OR APPLE)
    return()
endif()

# Shared settings for the scripts: the system-library allowlist used for bundling, so the
# install-tree test can check that nothing else resolves from outside the prefix.
set(DVE_INSTALL_TEST_SETTINGS "${PROJECT_BINARY_DIR}/dve_install_test_settings.cmake")
set(_dve_settings "set(DVE_SYSTEM_EXCLUDES [==[${DVE_RUNTIME_DEPENDENCY_SYSTEM_EXCLUDES}]==])\n")
string(APPEND _dve_settings "set(DVE_INSTALLED_COMPONENTS \"${DVE_INSTALLED_COMPONENTS}\")\n")
string(APPEND _dve_settings "set(DVE_INSTALLED_EXECUTABLES \"${DVE_INSTALLED_EXECUTABLES}\")\n")
string(APPEND _dve_settings "set(DVE_BUNDLEDIR \"${DVE_INSTALL_BUNDLEDIR}\")\n")
string(APPEND _dve_settings "set(DVE_LIBDIR \"${CMAKE_INSTALL_LIBDIR}\")\n")
string(APPEND _dve_settings "set(DVE_INCLUDEDIR \"${CMAKE_INSTALL_INCLUDEDIR}\")\n")
string(APPEND _dve_settings "set(DVE_BINDIR \"${CMAKE_INSTALL_BINDIR}\")\n")
string(APPEND _dve_settings "set(DVE_DATADIR \"${CMAKE_INSTALL_DATADIR}\")\n")
string(APPEND _dve_settings "set(DVE_EXPORTED_LIBRARIES \"${DVE_EXPORTED_LIBRARIES}\")\n")
string(APPEND _dve_settings "set(DVE_BUNDLE_DEPENDENCIES ${DVE_INSTALL_BUNDLE_RUNTIME_DEPENDENCIES})\n")
string(APPEND _dve_settings "set(DVE_SAMPLE_MAPS ${DVE_INSTALL_SAMPLE_MAPS})\n")
string(APPEND _dve_settings "set(DVE_VERSION \"${PROJECT_VERSION}\")\n")
string(APPEND _dve_settings "set(DVE_SOURCE_DIR \"${PROJECT_SOURCE_DIR}\")\n")
string(APPEND _dve_settings "set(DVE_BINARY_DIR \"${PROJECT_BINARY_DIR}\")\n")
string(APPEND _dve_settings "set(DVE_NOTICES_COMPONENTS \"${DVE_NOTICES_COMPONENTS}\")\n")
string(APPEND _dve_settings "set(DVE_NOTICES_INSTALL_DIR \"${DVE_NOTICES_INSTALL_DIR}\")\n")
string(APPEND _dve_settings "set(DVE_PYTHON3 \"${DVE_PYTHON3_EXECUTABLE}\")\n")
string(APPEND _dve_settings "set(DVE_NOTICES_TOOL \"${DVE_NOTICES_GENERATOR}\")\n")
string(APPEND _dve_settings "set(DVE_NOTICES_MANIFEST \"${DVE_NOTICES_MANIFEST}\")\n")
string(APPEND _dve_settings "set(DVE_SNDFILE_PROVIDER \"${DVE_SNDFILE_PROVIDER}\")\n")
# Shared libraries that must never be shipped (third_party/notices/manifest.json "forbidden").
string(APPEND _dve_settings "set(DVE_FORBIDDEN_LIBRARIES \"libmpg123;libmp3lame\")\n")
file(WRITE "${DVE_INSTALL_TEST_SETTINGS}" "${_dve_settings}")

set(DVE_INSTALL_TEST_PREFIX "${PROJECT_BINARY_DIR}/install_tests/prefix")
set(_dve_player_args "")
if(TARGET dve_player AND DEFINED DVE_PLAYER_SAMPLE_PAK)
    set(_dve_player_args -DPAK=${DVE_PLAYER_SAMPLE_PAK} -DBUILD_PLAYER=$<TARGET_FILE:dve_player>
        -DEXPECT=${_dve_player_hashes})
endif()
add_test(NAME dve_install_tree_test
    COMMAND ${CMAKE_COMMAND}
        -DSETTINGS=${DVE_INSTALL_TEST_SETTINGS}
        -DPREFIX=${DVE_INSTALL_TEST_PREFIX}
        -DWORK_DIR=${PROJECT_BINARY_DIR}/install_tests/work
        -DPROJECT=${PROJECT_SOURCE_DIR}/tests/data/player_sample
        ${_dve_player_args}
        -P ${PROJECT_SOURCE_DIR}/tests/cmake/dve_install_tree.cmake)
set_tests_properties(dve_install_tree_test PROPERTIES
    FIXTURES_SETUP dve_install_prefix
    TIMEOUT 900
    RESOURCE_LOCK dve_install_prefix)
if(_dve_player_args)
    set_tests_properties(dve_install_tree_test PROPERTIES
        FIXTURES_REQUIRED dve_player_sample_pak PROCESSORS 4)
endif()

if(DVE_INSTALL_DEVELOPMENT AND TARGET dve_player_runtime AND DEFINED DVE_PLAYER_SAMPLE_PAK)
    add_test(NAME dve_package_consumer_test
        COMMAND ${CMAKE_COMMAND}
            -DPREFIX=${DVE_INSTALL_TEST_PREFIX}
            -DSOURCE=${PROJECT_SOURCE_DIR}/tests/package_consumer
            -DWORK_DIR=${PROJECT_BINARY_DIR}/install_tests/consumer
            -DGENERATOR=${CMAKE_GENERATOR}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DVERSION=${PROJECT_VERSION}
            -DPAK=${DVE_PLAYER_SAMPLE_PAK}
            -DGAME_PROJECT=$<$<AND:$<TARGET_EXISTS:dve_player>,$<TARGET_EXISTS:dve_pack>>:${PROJECT_SOURCE_DIR}/tests/data/player_sample>
            -P ${PROJECT_SOURCE_DIR}/tests/cmake/dve_package_consumer.cmake)
    set_tests_properties(dve_package_consumer_test PROPERTIES
        FIXTURES_REQUIRED "dve_install_prefix;dve_player_sample_pak"
        RESOURCE_LOCK dve_install_prefix
        TIMEOUT 900)
endif()

# End-to-end game packaging from the installed prefix (Phase 4): package the sample game and
# an editor project, extract the tgz and run the shipped executable from elsewhere.
if(TARGET dve_player AND TARGET dve_pack AND TARGET dve_export_scene AND DVE_PYTHON3_EXECUTABLE
        AND "Runtime" IN_LIST DVE_NOTICES_COMPONENTS AND "Tools" IN_LIST DVE_INSTALLED_COMPONENTS)
    add_test(NAME dve_package_game_test
        COMMAND ${CMAKE_COMMAND}
            -DPREFIX=${DVE_INSTALL_TEST_PREFIX}
            -DPYTHON=${DVE_PYTHON3_EXECUTABLE}
            -DSCRIPT=${PROJECT_SOURCE_DIR}/scripts/dve_package_game.py
            -DNOTICES_TOOL=${DVE_NOTICES_GENERATOR}
            -DSAMPLE=${PROJECT_SOURCE_DIR}/tests/data/player_sample
            -DEDITOR_PROJECT=${PROJECT_SOURCE_DIR}/examples/editor_demo_project
            -DWORK_DIR=${PROJECT_BINARY_DIR}/install_tests/game_package
            -DBUILD_PLAYER=$<TARGET_FILE:dve_player>
            -DEXPECT=${_dve_player_hashes}
            -DSETTINGS=${DVE_INSTALL_TEST_SETTINGS}
            -P ${PROJECT_SOURCE_DIR}/tests/cmake/dve_package_game.cmake)
    set_tests_properties(dve_package_game_test PROPERTIES
        FIXTURES_REQUIRED dve_install_prefix
        RESOURCE_LOCK dve_install_prefix
        PROCESSORS 4
        TIMEOUT 1200)
endif()

# In-tree example of dve_add_game_package() (not part of ALL): build the sample game folder with
# `cmake --build <build> --target dve_sample_game_package`.
if(TARGET dve_player AND TARGET dve_pack AND DVE_PYTHON3_EXECUTABLE)
    dve_add_game_package(dve_sample_game_package
        PROJECT_DIR "${PROJECT_SOURCE_DIR}/tests/data/player_sample"
        OUTPUT_DIR "${PROJECT_BINARY_DIR}/game_packages/Player_Sample"
        TGZ VERIFY)
endif()

find_program(DVE_DPKG_DEB dpkg-deb)
add_test(NAME dve_cpack_test
    COMMAND ${CMAKE_COMMAND}
        -DSETTINGS=${DVE_INSTALL_TEST_SETTINGS}
        -DCPACK=${CMAKE_CPACK_COMMAND}
        -DCONFIG=${PROJECT_BINARY_DIR}/CPackConfig.cmake
        -DWORK_DIR=${PROJECT_BINARY_DIR}/install_tests/cpack
        -DDPKG_DEB=${DVE_DPKG_DEB}
        -P ${PROJECT_SOURCE_DIR}/tests/cmake/dve_cpack.cmake)
set_tests_properties(dve_cpack_test PROPERTIES LABELS slow TIMEOUT 1800 PROCESSORS 2)
