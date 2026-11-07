/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * ESP ZeroCode AI - Display (LVGL) Framework
 *
 * The framework brings the board's panel and touch up in its own task and
 * publishes LVGL's state here, so a product's own screen can wait for the
 * display instead of guessing with a fixed delay (or calling
 * app_display_init() a second time — LVGL has exactly one task).
 */

#pragma once

#include <stdbool.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_display_init(void);

/** LVGL is up and the panel has been registered with it. */
bool app_display_ready(void);

/** The display bring-up gave up (no panel, or LVGL never started). */
bool app_display_failed(void);

#ifdef __cplusplus
}
#endif
