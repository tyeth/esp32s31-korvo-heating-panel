#include "app_logic.h"
#include "app_driver.h"
#include "app_config.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_console.h>
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
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_system.h>

static const char *TAG = "diagnostics";

/* behaviors/uptime_log (EARLY_UPTIME) */
static void early_uptime_uptime_log_cb(void *arg)
{
    int64_t up_us = esp_timer_get_time();
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t min_heap  = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "early_uptime: up=%llds free_heap=%u min_heap=%u",
             (long long)(up_us / 1000000LL), (unsigned)free_heap, (unsigned)min_heap);
}

static void zc_early_uptime_init(void)
{
    const esp_timer_create_args_t early_uptime_args = {
        .callback = &early_uptime_uptime_log_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "early_uptime_up",
        .skip_unhandled_events = true,
    };
    esp_timer_handle_t early_uptime_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&early_uptime_args, &early_uptime_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(early_uptime_timer, (uint64_t)EARLY_UPTIME_UPTIME_LOG_S * 1000000ULL));
}

/* behaviors/task_watchdog (TASK_WATCHDOG) */
static void zc_task_watchdog_init(void)
{
    ESP_LOGI(TAG, "Task watchdog enabled (10s timeout, panics on starvation)");
}

/* behaviors/boot_count (BOOT_COUNT) */
static void zc_boot_count_init(void)
{
    nvs_handle_t boot_count_h;
    esp_err_t boot_count_err = nvs_open("app", NVS_READWRITE, &boot_count_h);
    if (boot_count_err == ESP_OK) {
        uint32_t boot_count_count = 0;
        nvs_get_u32(boot_count_h, "bootcount", &boot_count_count);
        boot_count_count++;
        nvs_set_u32(boot_count_h, "bootcount", boot_count_count);
        nvs_commit(boot_count_h);
        nvs_close(boot_count_h);
        ESP_LOGI(TAG, "Boot #%lu", (unsigned long)boot_count_count);
    } else {
        ESP_LOGW(TAG, "boot_count: nvs_open failed (%s)", esp_err_to_name(boot_count_err));
    }
}

/* behaviors/boot_reason_logger (BOOT_REASON_LOGGER) */
static void zc_boot_reason_logger_init(void)
{
    esp_reset_reason_t boot_reason_logger_r = esp_reset_reason();
    const char *boot_reason_logger_name = "unknown";
    switch (boot_reason_logger_r) {
        case ESP_RST_POWERON:   boot_reason_logger_name = "power-on";       break;
        case ESP_RST_EXT:       boot_reason_logger_name = "external";       break;
        case ESP_RST_SW:        boot_reason_logger_name = "software";       break;
        case ESP_RST_PANIC:     boot_reason_logger_name = "panic";          break;
        case ESP_RST_INT_WDT:   boot_reason_logger_name = "int-watchdog";   break;
        case ESP_RST_TASK_WDT:  boot_reason_logger_name = "task-watchdog";  break;
        case ESP_RST_WDT:       boot_reason_logger_name = "other-watchdog";break;
        case ESP_RST_DEEPSLEEP: boot_reason_logger_name = "deep-sleep-wake";break;
        case ESP_RST_BROWNOUT:  boot_reason_logger_name = "brownout";       break;
        case ESP_RST_SDIO:      boot_reason_logger_name = "sdio";           break;
        default: break;
    }
    ESP_LOGI(TAG, "Boot reason: %s (%d)", boot_reason_logger_name, (int)boot_reason_logger_r);
}

void diagnostics_logic_init(void)
{
    zc_early_uptime_init();
    zc_task_watchdog_init();
    zc_boot_count_init();
    zc_boot_reason_logger_init();
}
