# DVE_FETCH_LUA: build the pinned Lua 5.4 release from lua.org as a static library when no
# installed Lua 5.4 is found. Used by the Windows build (windows-msvc-player-release): vcpkg's
# "lua" port is Lua 5.5, which is not compatible with the 5.4 API DVE is written against, and a
# version override needs a manifest baseline. Linux presets keep using the distribution's
# liblua5.4 (this file is only included when DVE_FETCH_LUA is ON and nothing was found).
#
# Included at directory scope (enable_language must not run inside a function). Results:
#   DVE_LUA_TARGET       lua (static, built from the tarball's src/ without lua.c / luac.c)
#   DVE_LUA_FETCHED      ON
#   DVE_LUA_LICENSE_DIR  folder with LICENSE.txt, the license notice at the end of the
#                        tarball's lua.h (the release has no separate license file); the
#                        THIRD_PARTY_NOTICES generator reads it from there
# The headers are exposed as <lua5.4/lua.h> etc., the layout of Debian's liblua5.4-dev that
# src/game_script.cpp includes.
include_guard(GLOBAL)

set(DVE_LUA_FETCH_VERSION 5.4.9)
set(DVE_LUA_FETCH_SHA256 2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6)

enable_language(C)
include(FetchContent)
FetchContent_Declare(dve_lua
    URL "https://www.lua.org/ftp/lua-${DVE_LUA_FETCH_VERSION}.tar.gz"
    URL_HASH SHA256=${DVE_LUA_FETCH_SHA256}
    # No CMakeLists.txt in the tarball: populate only.
    SOURCE_SUBDIR dve_no_cmake_project)
FetchContent_MakeAvailable(dve_lua)

set(_dve_lua_src "${dve_lua_SOURCE_DIR}/src")
set(_dve_lua_sources
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c lobject.c
    lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c
    lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c loadlib.c loslib.c lstrlib.c
    ltablib.c lutf8lib.c linit.c)
list(TRANSFORM _dve_lua_sources PREPEND "${_dve_lua_src}/")
add_library(lua STATIC ${_dve_lua_sources})
set_target_properties(lua PROPERTIES LINKER_LANGUAGE C POSITION_INDEPENDENT_CODE ON)
set(_dve_lua_include "${CMAKE_BINARY_DIR}/_deps/dve_lua-include")
file(COPY "${_dve_lua_src}/lua.h" "${_dve_lua_src}/luaconf.h" "${_dve_lua_src}/lualib.h"
        "${_dve_lua_src}/lauxlib.h" "${_dve_lua_src}/lua.hpp"
    DESTINATION "${_dve_lua_include}/lua5.4")
target_include_directories(lua PRIVATE "${_dve_lua_src}" PUBLIC "$<BUILD_INTERFACE:${_dve_lua_include}>")
if(WIN32)
    target_compile_definitions(lua PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    target_compile_definitions(lua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(lua PUBLIC m ${CMAKE_DL_LIBS})
endif()

# The license notice at the end of lua.h ("Copyright (C) 1994-<year> Lua.org, PUC-Rio." and the
# MIT text), with the comment decoration removed.
file(STRINGS "${_dve_lua_src}/lua.h" _dve_lua_lines)
set(_dve_lua_license "")
set(_dve_lua_in_license OFF)
foreach(_dve_line IN LISTS _dve_lua_lines)
    if(_dve_line MATCHES "^\\* Copyright \\(C\\) [0-9-]+ Lua\\.org, PUC-Rio\\.")
        set(_dve_lua_in_license ON)
    endif()
    if(_dve_lua_in_license)
        if(_dve_line MATCHES "^\\*\\*\\*\\*")
            break()
        endif()
        string(REGEX REPLACE "^\\* ?" "" _dve_line "${_dve_line}")
        string(APPEND _dve_lua_license "${_dve_line}\n")
    endif()
endforeach()
if(NOT _dve_lua_license MATCHES "Permission is hereby granted")
    message(FATAL_ERROR "DVE_FETCH_LUA: could not read the license notice from ${_dve_lua_src}/lua.h")
endif()
set(DVE_LUA_LICENSE_DIR "${CMAKE_BINARY_DIR}/_deps/dve_lua-license")
file(WRITE "${DVE_LUA_LICENSE_DIR}/LICENSE.txt"
    "Lua ${DVE_LUA_FETCH_VERSION} (https://www.lua.org/ftp/lua-${DVE_LUA_FETCH_VERSION}.tar.gz, SHA256 ${DVE_LUA_FETCH_SHA256}),\n"
    "license notice from src/lua.h:\n\n${_dve_lua_license}")

set(DVE_LUA_TARGET lua)
set(DVE_LUA_FETCHED ON)
message(STATUS "Lua ${DVE_LUA_FETCH_VERSION}: built from source (DVE_FETCH_LUA), static")
