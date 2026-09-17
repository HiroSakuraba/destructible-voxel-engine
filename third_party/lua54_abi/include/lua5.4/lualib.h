/* Minimal Lua 5.4 standard-library ABI declarations for DVE. See lua.h. */
#ifndef DVE_LUA54_ABI_LUALIB_H
#define DVE_LUA54_ABI_LUALIB_H
#include "lua.h"
#ifdef __cplusplus
extern "C" {
#endif
void luaL_openlibs(lua_State* L);
#ifdef __cplusplus
}
#endif
#endif
