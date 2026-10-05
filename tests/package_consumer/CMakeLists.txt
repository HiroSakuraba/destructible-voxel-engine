# A tiny out-of-tree game built only against an installed dve package (dve_package_consumer_test).
cmake_minimum_required(VERSION 3.24)
project(dve_package_consumer LANGUAGES CXX)

set(DVE_REQUIRED_VERSION "" CACHE STRING "Version passed to find_package(dve)")
find_package(dve ${DVE_REQUIRED_VERSION} REQUIRED COMPONENTS core player_runtime)
message(STATUS "consumer: dve ${dve_VERSION} (${dve_VERSION_GIT_DESCRIBE}) at ${dve_DIR}; "
               "lua=${dve_HAVE_LUA} jolt=${dve_HAVE_JOLT} geometry=${dve_GEOMETRY_MODE}")

add_executable(tiny_game main.cpp)
target_link_libraries(tiny_game PRIVATE dve::player_runtime)

# Phase 4: dve_add_game_package() from the installed package (Runtime + Tools next to it).
set(DVE_CONSUMER_GAME_PROJECT "" CACHE PATH "Game project to package with dve_add_game_package()")
if(DVE_CONSUMER_GAME_PROJECT)
    dve_add_game_package(consumer_game_package
        PROJECT_DIR "${DVE_CONSUMER_GAME_PROJECT}"
        OUTPUT_DIR "${CMAKE_BINARY_DIR}/ConsumerGame"
        NAME ConsumerGame
        VERIFY)
endif()
