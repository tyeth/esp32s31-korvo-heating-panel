/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * Console verbosity: ONE level for the whole panel, stored in NVS.
 *
 * The build ships at VERBOSE (every tag, ours and the driver's, so a field
 * problem can be read off the console). A household does not want the
 * MicroSD/SPI-mem driver and the ADC oneshot driver on the same console, so the
 * settings page offers a single choice instead: ERROR, WARN, INFO, DEBUG or
 * VERBOSE. It is the default level for every tag, and the tags this panel runs
 * without ever reading are held below it (MSPI, ADC, the USB-serial one) — they
 * only appear in a VERBOSE dump from `log`.
 *
 * The choice is persisted, so a reboot comes back at the level that was picked,
 * and applied at boot BEFORE the board manager brings the peripherals up: the
 * noise is made during board bring-up, long before the settings page exists.
 */

#include "heating.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

static const char *TAG = "heating_log";

#define LOG_NVS_NAMESPACE   "zc_log"
#define LOG_NVS_KEY         "level"

typedef struct {
    const char      *name;
    esp_log_level_t  level;
} log_choice_t;

/* Index 0 is the build's own default. The order is the page's drop-down order. */
static const log_choice_t k_choices[] = {
    { "ERROR",   ESP_LOG_ERROR   },
    { "WARN",    ESP_LOG_WARN    },
    { "INFO",    ESP_LOG_INFO    },
    { "DEBUG",   ESP_LOG_DEBUG   },
    { "VERBOSE", ESP_LOG_VERBOSE },
};

/* Noisy ESP-IDF tags this product runs but never reads: they stay below the
 * chosen level (a VERBOSE selection still shows them, since everything does).
 * The board's LCD is on SPI2 and its ADC buttons on the oneshot driver, so both
 * tags log on every frame and every button poll. */
static const char *const k_quiet_tags[] = {
    "MSPI",
    "ADC",
};

static char s_level[8] = "VERBOSE";

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
    for (size_t i = 0; i < sizeof(k_quiet_tags) / sizeof(k_quiet_tags[0]); i++) {
        /* WARN is the ceiling for these: their INFO lines are the noise. */
        esp_log_level_t quiet = ESP_LOG_WARN;
        if (quiet > choice->level) quiet = choice->level;
        esp_log_level_set(k_quiet_tags[i], quiet);
    }
    ESP_LOGW(TAG, "console verbosity %s (MSPI/ADC held at WARN)", choice->name);
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
        /* Nothing chosen yet: keep the build level but already quieten the
         * driver tags, which is the reason this exists. */
        apply(choice_by_name(s_level) ? choice_by_name(s_level) : &k_choices[4]);
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
