/*
** Minimal Lua 5.4 C ABI declarations used by DVE's optional scripting host.
** Derived from the official Lua 5.4 public headers.
** Lua is Copyright (C) 1994-2025 Lua.org, PUC-Rio, under the MIT license.
** This compatibility header is only a fallback when a platform ships the Lua runtime
** library without the development headers. Prefer the platform's complete Lua headers.
*/
#ifndef DVE_LUA54_ABI_LUA_H
#define DVE_LUA54_ABI_LUA_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lua_State lua_State;
typedef double lua_Number;
typedef long long lua_Integer;
typedef unsigned long long lua_Unsigned;
typedef intptr_t lua_KContext;
typedef int (*lua_CFunction)(lua_State* L);
typedef int (*lua_KFunction)(lua_State* L, int status, lua_KContext ctx);

#define LUA_OK 0
#define LUA_TNONE (-1)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TLIGHTUSERDATA 2
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
#define LUA_TTABLE 5
#define LUA_TFUNCTION 6
#define LUA_TUSERDATA 7
#define LUA_TTHREAD 8
#define LUA_REGISTRYINDEX (-1001000)
#define LUA_MULTRET (-1)

void lua_close(lua_State* L);
int lua_gettop(lua_State* L);
int lua_absindex(lua_State* L, int idx);
void lua_settop(lua_State* L, int idx);
void lua_pushvalue(lua_State* L, int idx);
int lua_type(lua_State* L, int idx);
int lua_toboolean(lua_State* L, int idx);
int lua_isnumber(lua_State* L, int idx);
int lua_isstring(lua_State* L, int idx);
int lua_isinteger(lua_State* L, int idx);
lua_Number lua_tonumberx(lua_State* L, int idx, int* isnum);
lua_Integer lua_tointegerx(lua_State* L, int idx, int* isnum);
const char* lua_tolstring(lua_State* L, int idx, size_t* len);
void* lua_touserdata(lua_State* L, int idx);
void lua_pushnil(lua_State* L);
void lua_pushnumber(lua_State* L, lua_Number n);
void lua_pushinteger(lua_State* L, lua_Integer n);
const char* lua_pushstring(lua_State* L, const char* s);
const char* lua_pushlstring(lua_State* L, const char* s, size_t len);
void lua_pushboolean(lua_State* L, int b);
void lua_pushlightuserdata(lua_State* L, void* p);
int lua_getfield(lua_State* L, int idx, const char* k);
int lua_rawgeti(lua_State* L, int idx, lua_Integer n);
int lua_next(lua_State* L, int idx);
void lua_createtable(lua_State* L, int narr, int nrec);
void lua_setglobal(lua_State* L, const char* name);
void lua_setfield(lua_State* L, int idx, const char* k);
void lua_rawseti(lua_State* L, int idx, lua_Integer n);
int lua_pcallk(lua_State* L, int nargs, int nresults, int errfunc,
               lua_KContext ctx, lua_KFunction k);

#define lua_pop(L,n) lua_settop((L), -(n)-1)
#define lua_newtable(L) lua_createtable((L), 0, 0)
#define lua_tostring(L,i) lua_tolstring((L), (i), NULL)
#define lua_tonumber(L,i) lua_tonumberx((L), (i), NULL)
#define lua_tointeger(L,i) lua_tointegerx((L), (i), NULL)
#define lua_isnil(L,n) (lua_type((L), (n)) == LUA_TNIL)
#define lua_isboolean(L,n) (lua_type((L), (n)) == LUA_TBOOLEAN)
#define lua_istable(L,n) (lua_type((L), (n)) == LUA_TTABLE)
#define lua_isnoneornil(L,n) (lua_type((L), (n)) <= 0)
#define lua_pcall(L,n,r,f) lua_pcallk((L), (n), (r), (f), 0, NULL)

#ifdef __cplusplus
}
#endif
#endif
