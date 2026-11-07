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

static const char *TAG = "diagnostics";

/* behaviors/sntp_time (CLOCK) */
#if CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SOC_WIRELESS_HOST_SUPPORTED || CONFIG_ETH_ENABLED
static bool s_clock_synced = false;

/* Runs on the SNTP task, once per successful sync. Only the first one matters
 * to the product: after it, time() is trustworthy. */
static void clock_sntp_synced(struct timeval *tv)
{
    (void)tv;
    if (s_clock_synced) return;
    s_clock_synced = true;
    time_t now = time(NULL);
    struct tm lt = {};
    localtime_r(&now, &lt);
    char buf[32] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    ESP_LOGI(TAG, "clock: clock synced — local time %s (TZ %s)", buf, CLOCK_TZ);

}
#endif

static void zc_clock_init(void)
{
        /* The timezone applies to localtime() whether or not a sync ever lands. */
        setenv("TZ", CLOCK_TZ, 1);
        tzset();
    #if CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SOC_WIRELESS_HOST_SUPPORTED || CONFIG_ETH_ENABLED
        if (esp_sntp_enabled()) {
            /* A transport already runs SNTP (RainMaker does). Don't start a second
             * client — just observe its syncs. */
            esp_sntp_set_time_sync_notification_cb(clock_sntp_synced);
            ESP_LOGI(TAG, "clock: SNTP already running — observing");
        } else {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CLOCK_NTP_SERVER);
            cfg.start = true;              /* the client waits for an IP by itself */
            cfg.sync_cb = clock_sntp_synced;
            esp_err_t err = esp_netif_sntp_init(&cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "clock: SNTP init failed: %s — time() stays at the epoch", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "clock: SNTP started (%s)", CLOCK_NTP_SERVER);
            }
        }
    #else
        ESP_LOGW(TAG, "clock: no IP stack on this chip — clock stays unset");
    #endif
}

/* behaviors/boot_reason_logger (BOOT_REASON) */
static void zc_boot_reason_init(void)
{
    esp_reset_reason_t boot_reason_r = esp_reset_reason();
    const char *boot_reason_name = "unknown";
    switch (boot_reason_r) {
        case ESP_RST_POWERON:   boot_reason_name = "power-on";       break;
        case ESP_RST_EXT:       boot_reason_name = "external";       break;
        case ESP_RST_SW:        boot_reason_name = "software";       break;
        case ESP_RST_PANIC:     boot_reason_name = "panic";          break;
        case ESP_RST_INT_WDT:   boot_reason_name = "int-watchdog";   break;
        case ESP_RST_TASK_WDT:  boot_reason_name = "task-watchdog";  break;
        case ESP_RST_WDT:       boot_reason_name = "other-watchdog";break;
        case ESP_RST_DEEPSLEEP: boot_reason_name = "deep-sleep-wake";break;
        case ESP_RST_BROWNOUT:  boot_reason_name = "brownout";       break;
        case ESP_RST_SDIO:      boot_reason_name = "sdio";           break;
        default: break;
    }
    ESP_LOGI(TAG, "Boot reason: %s (%d)", boot_reason_name, (int)boot_reason_r);
}

/* behaviors/boot_count (BOOT_CNT) */
static void zc_boot_cnt_init(void)
{
    nvs_handle_t boot_cnt_h;
    esp_err_t boot_cnt_err = nvs_open("app", NVS_READWRITE, &boot_cnt_h);
    if (boot_cnt_err == ESP_OK) {
        uint32_t boot_cnt_count = 0;
        nvs_get_u32(boot_cnt_h, "bootcount", &boot_cnt_count);
        boot_cnt_count++;
        nvs_set_u32(boot_cnt_h, "bootcount", boot_cnt_count);
        nvs_commit(boot_cnt_h);
        nvs_close(boot_cnt_h);
        ESP_LOGI(TAG, "Boot #%lu", (unsigned long)boot_cnt_count);
    } else {
        ESP_LOGW(TAG, "boot_count: nvs_open failed (%s)", esp_err_to_name(boot_cnt_err));
    }
}

/* behaviors/task_watchdog (WDT) */
static void zc_wdt_init(void)
{
    ESP_LOGI(TAG, "Task watchdog enabled (10s timeout, panics on starvation)");
}

void diagnostics_logic_init(void)
{
    zc_clock_init();
    zc_boot_reason_init();
    zc_boot_cnt_init();
    zc_wdt_init();
}
