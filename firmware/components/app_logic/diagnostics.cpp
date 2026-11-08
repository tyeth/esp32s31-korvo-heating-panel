#include "app_logic.h"
#include "app_driver.h"
#include "app_config.h"
#include "app_mqtt.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include "esp_board_manager.h"
#include "dev_button.h"
#include <iot_button.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <time.h>
#include <stdlib.h>
#include <esp_event.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
    /* Set the zone now — localtime()/strftime() must work from the first call.
     * The SNTP client itself is started by diagnostics_sntp_start() from a tiny
     * task: it waits for the default event loop, which the transport block
     * creates around the same time, and must not depend on who wins that race. */
    setenv("TZ", CLOCK_TZ, 1);
    tzset();
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
    /* Chatter from the app/tag level is verbosity, not a fault — a VERBOSE
     * build should not reboot the panel for a slow log line. Idle-task
     * starvation still panics, which is the check that matters. */
    ESP_LOGI(TAG, "Task watchdog enabled (10s timeout, log only; idle starvation still panics)");
}

void diagnostics_logic_init(void)
{
    zc_boot_reason_init();
    zc_boot_cnt_init();
    zc_wdt_init();
}

/* ── the wall clock's time source ──────────────────────────────────────
 *
 * esp_netif_sntp_init() needs the default event loop, and on this product the
 * transport block (app_mqtt_init) is what creates it — after the behaviours
 * run. Starting the client on a task that waits for the loop removes that
 * ordering race: whoever creates the loop first, SNTP comes up a moment later.
 */
#if CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SOC_WIRELESS_HOST_SUPPORTED || CONFIG_ETH_ENABLED
static void sntp_start_task(void *arg)
{
    (void)arg;
    /* The default event loop must exist before SNTP starts. Calling
     * esp_event_loop_create_default() is safe in either order: it creates the
     * loop if the transport block has not yet, and returns ESP_ERR_INVALID_STATE
     * if it already has. */
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "clock: default event loop unavailable (%s)", esp_err_to_name(loop_err));
    }

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
    vTaskDelete(NULL);
}
#endif

/* Every 60 s: the link state the panel shows, the clock, and the heap — the
 * three facts a field report needs when a panel sits at "offline" or looks
 * idle while the broker is actually up. */
static void status_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        struct tm lt = {};
        time_t t = time(NULL);
        bool clock_ok = t > 1600000000;
        if (clock_ok) localtime_r(&t, &lt);
        ESP_LOGI(TAG, "status: broker %s, clock %s%02d:%02d, heap %u (min %u)",
                 app_mqtt_configured() ? "configured" : "not configured",
                 clock_ok ? "" : "unset ", lt.tm_hour, lt.tm_min,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
    }
}

void diagnostics_sntp_start(void)
{
#if CONFIG_SOC_WIFI_SUPPORTED || CONFIG_SOC_WIRELESS_HOST_SUPPORTED || CONFIG_ETH_ENABLED
    if (xTaskCreate(sntp_start_task, "sntp_start", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "clock: SNTP task not started — time() stays at the epoch");
    }
    if (xTaskCreate(status_task, "status_log", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "status: periodic status log not started");
    }
#else
    ESP_LOGW(TAG, "clock: no IP stack on this chip — clock stays unset");
#endif
}

/* app_main calls this once the network stack exists: it sets the zone and
 * starts the SNTP client on its own task (see sntp_start_task above), so the
 * clock never depends on who created the default event loop first. */
void diagnostics_clock_init(void)
{
    zc_clock_init();
    diagnostics_sntp_start();
}
