/* Minimal Lua 5.4 auxiliary-library ABI declarations for DVE. See lua.h. */
#ifndef DVE_LUA54_ABI_LAUXLIB_H
#define DVE_LUA54_ABI_LAUXLIB_H
#include "lua.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct luaL_Reg {
    const char* name;
    lua_CFunction func;
} luaL_Reg;

lua_State* luaL_newstate(void);
lua_Integer luaL_checkinteger(lua_State* L, int arg);
lua_Number luaL_checknumber(lua_State* L, int arg);
const char* luaL_checklstring(lua_State* L, int arg, size_t* len);
void luaL_checktype(lua_State* L, int arg, int type);
int luaL_error(lua_State* L, const char* fmt, ...);
lua_Integer luaL_len(lua_State* L, int idx);
int luaL_loadbufferx(lua_State* L, const char* buff, size_t size,
                     const char* name, const char* mode);
lua_Number luaL_optnumber(lua_State* L, int arg, lua_Number defaultValue);
int luaL_ref(lua_State* L, int tableIndex);
void luaL_unref(lua_State* L, int tableIndex, int ref);
void luaL_setfuncs(lua_State* L, const luaL_Reg* functions, int upvalues);

#define luaL_checkstring(L,n) luaL_checklstring((L), (n), NULL)
#define luaL_loadbuffer(L,s,sz,n) luaL_loadbufferx((L), (s), (sz), (n), NULL)

#ifdef __cplusplus
}
#endif
#endif
