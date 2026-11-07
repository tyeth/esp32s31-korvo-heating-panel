/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * ESP ZeroCode AI - Application Entry Point
 *
 * Protocol solutions are blocks: the generator copies only the selected
 * frameworks' components into the build and fills the main_includes /
 * main_init slots below from each protocol block's contributions. A
 * Matter-only product never references RainMaker / Zigbee / BLE Mesh.
 *
 * ──── INITIALIZATION ORDER ────────────────────────────────────────────
 * 1. NVS Flash       → Persistent storage (required by all frameworks)
 * 1b. Board manager  → brings up the 'esp32_s31_korvo_1' board's on-board
 *                      hardware, BEFORE the drivers that may ask it for a handle
 * 2. Hardware driver → GPIO, I2C, SPI peripherals + callback registry
 * 3. Framework(s)    → from product.yml `frameworks:` field
 * 4. Business logic  → State machines, automation, button handlers
 * 5. Device console  → CLI for testing (after frameworks are up)
 * ─────────────────────────────────────────────────────────────────────
 */

#include "app_driver.h"
#include "esp_board_manager.h"

#include "app_audio.h"

#include "app_display.h"

#include "app_mqtt.h"

#include "app_webui.h"
#include "app_logic.h"
#include "app_console.h"

#include <esp_log.h>
#include <nvs_flash.h>

static const char *TAG = "app_main";

extern "C" void app_main(void)
{
    /* Initialize NVS */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* On-board hardware from the 'esp32_s31_korvo_1' board definition — before the
       drivers, which may ask the board for a handle. */
    ESP_ERROR_CHECK(esp_board_manager_init());

    /* Initialize hardware drivers */
    ESP_ERROR_CHECK(app_driver_init());

    /* Initialize protocol solutions */
    /* Degrades rather than aborts: no PSRAM (or no `model` partition) leaves
     * voice unable to start, and a device whose other functions still work is
     * better than one that reset-loops. app_audio_init logs the reason. */
    {
        esp_err_t audio_err = app_audio_init();
        if (audio_err != ESP_OK) {
            ESP_LOGE(TAG, "voice disabled: %s", esp_err_to_name(audio_err));
        }
    }

    ESP_ERROR_CHECK(app_display_init());

    ESP_ERROR_CHECK(app_mqtt_init());

    ESP_ERROR_CHECK(app_webui_init());

    /* Composed behaviors (generated, zc_behaviors.cpp) — before the product's
     * own logic, so it can rely on persistence, console, diagnostics being up */
    ESP_ERROR_CHECK(zc_behaviors_init());

    /* Initialize business logic */
    ESP_ERROR_CHECK(app_logic_init());

    /* Start device console */
    ESP_ERROR_CHECK(app_console_init());

    ESP_LOGI(TAG, "ZeroCode AI firmware started");
}
