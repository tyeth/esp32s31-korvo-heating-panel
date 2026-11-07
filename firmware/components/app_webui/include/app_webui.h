/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * ESP ZeroCode AI - Web UI Framework
 *
 * BASE: No-op stub. The generator replaces app_webui.cpp with the real
 * implementation when the product has device-type bindings.
 */

#pragma once

#include <esp_err.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_webui_init(void);

/** Install the page handler that serves the product's own settings page at
 *  "/settings" (GET renders it, POST applies a form). Call it before
 *  app_webui_init(); without one the device serves only the state page. */
void app_webui_set_settings_handler(esp_err_t (*handler)(void));

/* Written against nothing but this header — no esp_http_server type leaks into
 * the product. The handler answers the request being served: set the type,
 * send the page in chunks, end it; on a POST read the body first. */
void app_webui_chunk_begin(const char *content_type);
void app_webui_send_chunk(const char *text);
void app_webui_chunk_end(void);
/** Read up to @p len bytes of the request body; < 0 when there is none left. */
int  app_webui_read_request(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
