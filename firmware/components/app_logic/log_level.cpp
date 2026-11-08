/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Console verbosity: ONE level for the whole panel, stored in NVS.
 *
 * The build default is INFO for the IDF components (sdkconfig.defaults) — the
 * MIPI/DSI, LCD, ADC-oneshot and SPI-mem drivers are never ours to read. On top
 * of it this file decides three things at boot, before the board manager starts
 * any peripheral:
 *
 *   - the panel's OWN tags log at DEBUG (they are the ones worth reading);
 *   - the noisy IDF tags (MSPI, the LCD/DSI traffic, ADC oneshot) are held at
 *     WARN, so even a VERBOSE selection does not bury the product's lines;
 *   - "*" is set to what the household picked on the settings page, or INFO.
 *
 * The choice is persisted, so a reboot comes back at the level that was picked.
 * `log * VERBOSE` on the console and the settings page's drop-down are the two
 * ways to change it.
 */

#include "heating.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

/* !CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT: the maximum level is above the default,
 * so a tag can be raised at runtime. With the maximum pinned to the default,
 * every esp_log_level_set() above it — this file's own, `log * VERBOSE`, the
 * settings page's drop-down — would be a silent no-op. */
#if CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT
#error "CONFIG_LOG_MAXIMUM_EQUALS_DEFAULT pins the maximum to the default: dynamic log levels cannot raise any tag"
#endif

static const char *TAG = "heating_log";

#define LOG_NVS_NAMESPACE   "zc_log"
#define LOG_NVS_KEY         "level"

typedef struct {
    const char      *name;
    esp_log_level_t  level;
} log_choice_t;

/* The order is the settings page's drop-down order; INFO is the default. */
static const log_choice_t k_choices[] = {
    { "ERROR",   ESP_LOG_ERROR   },
    { "WARN",    ESP_LOG_WARN    },
    { "INFO",    ESP_LOG_INFO    },
    { "DEBUG",   ESP_LOG_DEBUG   },
    { "VERBOSE", ESP_LOG_VERBOSE },
};

/* Noisy ESP-IDF tags this product runs but never reads. Held at WARN whatever
 * the selection is: their INFO/DEBUG lines repeat on every frame, every button
 * poll and every timing tune, and none of it says anything about the panel. */
static const char *const k_quiet_tags[] = {
    "MSPI",             /* SPI-mem + PSRAM timing, flash access */
    "ADC",              /* the board's ADC button group, polled constantly */
    "esp_lcd",          /* panel init / draw traffic */
    "lcd",              /* panel + RGB/DSI driver */
    "lcd_panel",        /* panel driver */
    "dsi",              /* MIPI-DSI */
    "mipi",             /* MIPI */
    "esp_ldo",          /* the DSI PHY's regulator */
};

/* The panel's own tags — every one a file in this product logs under. They get
 * DEBUG when the selection allows it, so the product's behaviour is visible
 * without turning all of esp-idf up. */
static const char *const k_own_tags[] = {
    "app_main",
    "heating",
    "heating_ui",
    "heating_web",
    "heating_voice",
    "heating_mqtt",
    "heating_panel",
    "heating_log",
    "diagnostics",
    "control",
    "button_registry",
    "zc_behaviors",
    "app_audio",
    "app_display",
    "app_driver",
    "app_driver_cb",
    "app_mqtt",
    "app_console",
    "app_webui",
};

static char s_level[8] = "INFO";

static const log_choice_t *choice_by_name(const char *name)
{
    if (name == NULL) return NULL;
    for (size_t i = 0; i < sizeof(k_choices) / sizeof(k_choices[0]); i++) {
        if (strcasecmp(name, k_choices[i].name) == 0) return &k_choices[i];
    }
    return NULL;
}

static void apply(const log_choice_t *choice)
{
    esp_log_level_set("*", choice->level);

    /* Our own tags: DEBUG, but never above what was asked for. */
    esp_log_level_t own = ESP_LOG_DEBUG;
    if (own > choice->level) own = choice->level;
    for (size_t i = 0; i < sizeof(k_own_tags) / sizeof(k_own_tags[0]); i++) {
        esp_log_level_set(k_own_tags[i], own);
    }

    /* The noise stays at WARN whatever was picked. */
    for (size_t i = 0; i < sizeof(k_quiet_tags) / sizeof(k_quiet_tags[0]); i++) {
        esp_log_level_set(k_quiet_tags[i], ESP_LOG_WARN);
    }
    ESP_LOGI(TAG, "console verbosity %s — %u panel tags at DEBUG, %u driver tags at WARN",
             choice->name, (unsigned)(sizeof(k_own_tags) / sizeof(k_own_tags[0])),
             (unsigned)(sizeof(k_quiet_tags) / sizeof(k_quiet_tags[0])));
}

void heating_log_level_apply_boot(void)
{
    nvs_handle_t h;
    char stored[16] = {0};
    size_t len = sizeof(stored);
    if (nvs_open(LOG_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, LOG_NVS_KEY, stored, &len) != ESP_OK) stored[0] = '\0';
        nvs_close(h);
    }

    const log_choice_t *choice = choice_by_name(stored);
    if (choice == NULL) {
        /* Nothing chosen yet: the build's INFO, with the driver tags quietened
         * whatever the selection — that is the whole point of this file. */
        apply(choice_by_name("INFO"));
        return;
    }
    snprintf(s_level, sizeof(s_level), "%s", choice->name);
    apply(choice);
}

const char *heating_log_level_get(void)
{
    return s_level;
}

esp_err_t heating_log_level_set(const char *name)
{
    const log_choice_t *choice = choice_by_name(name);
    if (choice == NULL) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(LOG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, LOG_NVS_KEY, choice->name);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    snprintf(s_level, sizeof(s_level), "%s", choice->name);
    apply(choice);
    return ESP_OK;
}
