#include "app_logic.h"
#include "app_driver.h"
#include "app_config.h"
#include <esp_log.h>
#include "esp_board_manager.h"
#include "dev_button.h"
#include <iot_button.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <time.h>
#include <stdlib.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_console.h>
#include <esp_timer.h>
#include <inttypes.h>
#include <ctype.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_psram.h>
#include <sdkconfig.h>
#include <esp_app_desc.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <string.h>
#include <strings.h>
#include <esp_heap_caps.h>
#include <nvs.h>

static const char *TAG = "control";

/* drivers/board_button (SET_BTN) */
/* set_btn: the board's own 'adc_button_group' button device.
 * No GPIO number appears anywhere in this product — the pin and active level
 * are the BOARD's facts, and claiming the pin here would be an IO conflict
 * with the board's own peripheral. dev_button is built ON iot_button (its
 * handles ARE button_handle_t), so these are ordinary iot_button callbacks on
 * a device somebody else created.
 *
 * Gesture hooks: weak, so the product overrides any of them with a plain
 * definition of the same signature in its own file. They run on the button
 * timer task — keep them short and non-blocking. */
__attribute__((weak)) void set_btn_on_single_click(void)
{
#ifndef SET_BTN_SINGLE_CLICK_TOGGLES
    ESP_LOGI(TAG, "set_btn: single click (no handler)");
#endif
    /* With target_param bound the click already toggled it — nothing more. */
}
__attribute__((weak)) void set_btn_on_double_click(void)
{
    ESP_LOGI(TAG, "set_btn: double click (no handler)");
}
__attribute__((weak)) void set_btn_on_long_press(void)
{
    ESP_LOGI(TAG, "set_btn: long press (no handler)");
}

static void set_btn_board_button_cb(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    set_btn_on_single_click();
}
static void set_btn_double_click_cb(void *arg, void *usr_data) { (void)arg; (void)usr_data; set_btn_on_double_click(); }
static void set_btn_long_press_cb(void *arg, void *usr_data)   { (void)arg; (void)usr_data; set_btn_on_long_press(); }

static void zc_set_btn_init(void)
{
    /* esp_board_manager_init() ran in app_main BEFORE app_logic_init(), so
       the iot_button device for 'adc_button_group' already exists — this asks
       for its handle and registers callbacks; it creates NOTHING. Going
       through app_button_get_or_create() here would make a SECOND iot_button
       device on a pin the board's device already owns — the exact race the
       registry exists to prevent. */
    void *set_btn_h = NULL;
    if (esp_board_manager_get_device_handle("adc_button_group", &set_btn_h) != ESP_OK || set_btn_h == NULL) {
        /* Generation refuses a device the board does not declare, so reaching
           here means the board manager failed to bring it up at boot.
           Degraded, not fatal: the product keeps running without this input. */
        ESP_LOGE(TAG, "set_btn: board device 'adc_button_group' has no handle — button disabled");
    } else {
        dev_button_handles_t *set_btn_btns = (dev_button_handles_t *)set_btn_h;
        if (set_btn_btns->num_buttons < 1 || set_btn_btns->button_handles[0] == NULL) {
            ESP_LOGE(TAG, "set_btn: board device 'adc_button_group' carries no button handle — button disabled");
        } else {
            button_handle_t set_btn_btn = set_btn_btns->button_handles[0];
            bool set_btn_bound = true;
            if (iot_button_register_cb(set_btn_btn, BUTTON_SINGLE_CLICK, NULL, set_btn_board_button_cb, NULL) != ESP_OK) {
                ESP_LOGE(TAG, "set_btn: single click not bound — check the button timings (a hold must be longer than the double-click window)");
                set_btn_bound = false;
            }
            if (iot_button_register_cb(set_btn_btn, BUTTON_DOUBLE_CLICK, NULL, set_btn_double_click_cb, NULL) != ESP_OK) {
                ESP_LOGE(TAG, "set_btn: double click not bound — check the button timings (a hold must be longer than the double-click window)");
                set_btn_bound = false;
            }
            /* Long-press thresholds are PER CALLBACK (event_args), so the hook
               and the factory reset each keep their own hold time. */
            button_event_args_t set_btn_lp = {};
            set_btn_lp.long_press.press_time = 2000;
            if (iot_button_register_cb(set_btn_btn, BUTTON_LONG_PRESS_START, &set_btn_lp, set_btn_long_press_cb, NULL) != ESP_OK) {
                ESP_LOGE(TAG, "set_btn: long press not bound — check the button timings (a hold must be longer than the double-click window)");
                set_btn_bound = false;
            }
            if (set_btn_bound) {
                ESP_LOGI(TAG, "set_btn: board button 'adc_button_group' bound (click / double click / long press 2000 ms)");
            }
        }
    }
}

/* drivers/board_button (BOOT_BTN) */
/* boot_btn: the board's own 'boot_button' button device.
 * No GPIO number appears anywhere in this product — the pin and active level
 * are the BOARD's facts, and claiming the pin here would be an IO conflict
 * with the board's own peripheral. dev_button is built ON iot_button (its
 * handles ARE button_handle_t), so these are ordinary iot_button callbacks on
 * a device somebody else created.
 *
 * Gesture hooks: weak, so the product overrides any of them with a plain
 * definition of the same signature in its own file. They run on the button
 * timer task — keep them short and non-blocking. */
__attribute__((weak)) void boot_btn_on_single_click(void)
{
#ifndef BOOT_BTN_SINGLE_CLICK_TOGGLES
    ESP_LOGI(TAG, "boot_btn: single click (no handler)");
#endif
    /* With target_param bound the click already toggled it — nothing more. */
}
__attribute__((weak)) void boot_btn_on_double_click(void)
{
    ESP_LOGI(TAG, "boot_btn: double click (no handler)");
}
__attribute__((weak)) void boot_btn_on_long_press(void)
{
    ESP_LOGI(TAG, "boot_btn: long press (no handler)");
}

static void boot_btn_board_button_cb(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    boot_btn_on_single_click();
}
static void boot_btn_double_click_cb(void *arg, void *usr_data) { (void)arg; (void)usr_data; boot_btn_on_double_click(); }
static void boot_btn_long_press_cb(void *arg, void *usr_data)   { (void)arg; (void)usr_data; boot_btn_on_long_press(); }

static void zc_boot_btn_init(void)
{
    /* esp_board_manager_init() ran in app_main BEFORE app_logic_init(), so
       the iot_button device for 'boot_button' already exists — this asks
       for its handle and registers callbacks; it creates NOTHING. Going
       through app_button_get_or_create() here would make a SECOND iot_button
       device on a pin the board's device already owns — the exact race the
       registry exists to prevent. */
    void *boot_btn_h = NULL;
    if (esp_board_manager_get_device_handle("boot_button", &boot_btn_h) != ESP_OK || boot_btn_h == NULL) {
        /* Generation refuses a device the board does not declare, so reaching
           here means the board manager failed to bring it up at boot.
           Degraded, not fatal: the product keeps running without this input. */
        ESP_LOGE(TAG, "boot_btn: board device 'boot_button' has no handle — button disabled");
    } else {
        dev_button_handles_t *boot_btn_btns = (dev_button_handles_t *)boot_btn_h;
        if (boot_btn_btns->num_buttons < 1 || boot_btn_btns->button_handles[0] == NULL) {
            ESP_LOGE(TAG, "boot_btn: board device 'boot_button' carries no button handle — button disabled");
        } else {
            button_handle_t boot_btn_btn = boot_btn_btns->button_handles[0];
            bool boot_btn_bound = true;
            if (iot_button_register_cb(boot_btn_btn, BUTTON_SINGLE_CLICK, NULL, boot_btn_board_button_cb, NULL) != ESP_OK) {
                ESP_LOGE(TAG, "boot_btn: single click not bound — check the button timings (a hold must be longer than the double-click window)");
                boot_btn_bound = false;
            }
            if (iot_button_register_cb(boot_btn_btn, BUTTON_DOUBLE_CLICK, NULL, boot_btn_double_click_cb, NULL) != ESP_OK) {
                ESP_LOGE(TAG, "boot_btn: double click not bound — check the button timings (a hold must be longer than the double-click window)");
                boot_btn_bound = false;
            }
            /* Long-press thresholds are PER CALLBACK (event_args), so the hook
               and the factory reset each keep their own hold time. */
            button_event_args_t boot_btn_lp = {};
            boot_btn_lp.long_press.press_time = 2000;
            if (iot_button_register_cb(boot_btn_btn, BUTTON_LONG_PRESS_START, &boot_btn_lp, boot_btn_long_press_cb, NULL) != ESP_OK) {
                ESP_LOGE(TAG, "boot_btn: long press not bound — check the button timings (a hold must be longer than the double-click window)");
                boot_btn_bound = false;
            }
            if (boot_btn_bound) {
                ESP_LOGI(TAG, "boot_btn: board button 'boot_button' bound (click / double click / long press 2000 ms)");
            }
        }
    }
}

void control_logic_init(void)
{
    zc_set_btn_init();
    zc_boot_btn_init();
}
