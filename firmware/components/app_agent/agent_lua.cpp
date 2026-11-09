/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "agent_lua.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

static const char *TAG = "agent_lua";

/* One run's context, carried through the Lua registry. */
typedef struct {
    const app_agent_ops_t *ops;
    char   *out;
    size_t  out_len;
    size_t  out_w;
} lua_ctx_t;

static lua_ctx_t *ctx_of(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "agent_ctx");
    lua_ctx_t *c = (lua_ctx_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return c;
}

static void ctx_print(lua_ctx_t *c, const char *text)
{
    ESP_LOGI(TAG, "script: %s", text);
    if (c->out_w + 2 < c->out_len) {
        c->out_w += (size_t)snprintf(c->out + c->out_w, c->out_len - c->out_w,
                                     "%s%s", c->out_w ? "\n" : "", text);
    }
}

static int l_log(lua_State *L)
{
    lua_ctx_t *c = ctx_of(L);
    const char *s = luaL_checkstring(L, 1);
    if (c) ctx_print(c, s);
    return 0;
}

static int l_heating_set(lua_State *L)
{
    lua_ctx_t *c = ctx_of(L);
    if (c == NULL || c->ops == NULL) return luaL_error(L, "no device ops");
    int idx = -1;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        idx = (int)lua_tointeger(L, 1) - 1;            /* scripts count from 1 */
    } else {
        idx = c->ops->find_room(luaL_checkstring(L, 1));
    }
    float celsius = (float)luaL_checknumber(L, 2);
    if (idx < 0 || idx >= c->ops->room_count()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no such room");
        return 2;
    }
    int rc = c->ops->set_target(idx, celsius);
    char msg[80];
    snprintf(msg, sizeof(msg), "%s -> %.1f: %s", c->ops->room_name(idx), celsius,
             rc == 0 ? "sent" : rc == 2 ? "already set" : rc == 1 ? "offline" : "refused");
    ctx_print(c, msg);
    lua_pushboolean(L, rc == 0 || rc == 2);
    lua_pushstring(L, msg);
    return 2;
}

static int l_heating_rooms(lua_State *L)
{
    lua_ctx_t *c = ctx_of(L);
    if (c == NULL || c->ops == NULL) return luaL_error(L, "no device ops");
    lua_newtable(L);
    for (int i = 0; i < c->ops->room_count(); i++) {
        lua_newtable(L);
        lua_pushstring(L, c->ops->room_name(i));
        lua_setfield(L, -2, "name");
        lua_pushinteger(L, i + 1);
        lua_setfield(L, -2, "index");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* A 400k-instruction budget: a script that spins never hangs the recogniser's
 * task (which is where the agent, and this sandbox, run). */
static void budget_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;
    luaL_error(L, "instruction budget exhausted");
}

int agent_lua_run(const app_agent_ops_t *ops, const char *script,
                  char *out, size_t out_len)
{
    if (out == NULL || out_len < 8) return -1;
    out[0] = '\0';
    lua_ctx_t ctx = { ops, out, out_len, 0 };

    lua_State *L = luaL_newstate();
    if (L == NULL) {
        snprintf(out, out_len, "lua: out of memory");
        return -1;
    }
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);      lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);  lua_pop(L, 1);
    /* No io, no os, no package, no load-from-file: the sandbox's whole world
     * is the bindings below. dofile/loadfile would read a filesystem the
     * script has no business in. */
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");

    lua_pushlightuserdata(L, &ctx);
    lua_setfield(L, LUA_REGISTRYINDEX, "agent_ctx");

    lua_newtable(L);
    lua_pushcfunction(L, l_heating_set);   lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_heating_rooms); lua_setfield(L, -2, "rooms");
    lua_setglobal(L, "heating");
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "log");

    lua_sethook(L, budget_hook, LUA_MASKCOUNT, 400000);

    int rc = luaL_loadstring(L, script);
    if (rc == LUA_OK) rc = lua_pcall(L, 0, 0, 0);
    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        ESP_LOGW(TAG, "script failed: %s", err ? err : "?");
        size_t w = strlen(out);
        snprintf(out + w, out_len - w, "%serror: %s", w ? "\n" : "", err ? err : "?");
    }
    lua_close(L);
    return rc == LUA_OK ? 0 : -1;
}
