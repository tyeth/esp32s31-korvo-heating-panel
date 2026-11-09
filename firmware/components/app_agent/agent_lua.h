/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/** The agent's Lua 5.4 sandbox (the run_lua tool). Exposes to scripts:
 *    heating.set(room, celsius)  room = name or 1-based index; returns bool, msg
 *    heating.rooms()             array of {name=, index=}
 *    log(text)                   into the tool result and the serial log
 *  plus base/table/string/math. No io, no os, no package; a 400k-instruction
 *  budget kills runaway loops. Output (log lines + errors) lands in out. */

#pragma once

#include <stddef.h>
#include "app_agent.h"

#ifdef __cplusplus
extern "C" {
#endif

int agent_lua_run(const app_agent_ops_t *ops, const char *script,
                  char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
