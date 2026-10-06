cmake_minimum_required(VERSION 3.24)
# dve_player's link closure (written at configure time) must contain the player runtime and
# must not contain the editor library.
file(STRINGS "${CLOSURE}" closure)
if(NOT "dve_player_runtime" IN_LIST closure)
    message(FATAL_ERROR "dve_player_runtime missing from the player link closure: ${closure}")
endif()
foreach(forbidden dve_editor dve_editor_sdl_canvas)
    if("${forbidden}" IN_LIST closure)
        message(FATAL_ERROR "dve_player links ${forbidden}: ${closure}")
    endif()
endforeach()
list(JOIN closure ", " text)
message(STATUS "dve_player link closure: ${text}")
