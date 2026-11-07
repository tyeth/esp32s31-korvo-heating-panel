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

/* behaviors/console_uptime (CON_UPTIME) */
static int con_uptime_cmd(int argc, char **argv)
{
    uint64_t us = esp_timer_get_time();
    uint64_t s  = us / 1000000ULL;
    uint32_t d  = (uint32_t)(s / 86400);
    uint32_t h  = (uint32_t)((s % 86400) / 3600);
    uint32_t m  = (uint32_t)((s % 3600) / 60);
    uint32_t sec = (uint32_t)(s % 60);
    /* NEWLIB NANO printf (the default) has no 64-bit support — PRIu64 prints
     * a literal "lu". Total seconds fits u32 for 136 years; cast it. */
    printf("uptime: %" PRIu32 "d %02" PRIu32 "h %02" PRIu32 "m %02" PRIu32 "s  (%" PRIu32 " s)\n",
           d, h, m, sec, (uint32_t)s);
    return 0;
}

static void zc_con_uptime_init(void)
{
    /* Zero-init + assign: newer IDF adds fields to esp_console_cmd_t and the
     * build runs with -Werror=missing-field-initializers. */
    esp_console_cmd_t con_uptime_c = {};
    con_uptime_c.command = "uptime";
    con_uptime_c.help = "Seconds since boot";
    con_uptime_c.func = &con_uptime_cmd;
    esp_console_cmd_register(&con_uptime_c);
}

/* behaviors/console_chip_info (CON_CHIP) */
static int con_chip_cmd(int argc, char **argv)
{
    esp_chip_info_t info = {};
    esp_chip_info(&info);
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);

    /* The bootloader refuses an image built for another chip, so the build
     * target IS the running chip: "esp32s31" -> "ESP32-S31". */
    char model[16];
    const char *target = CONFIG_IDF_TARGET;
    size_t n = 0;
    for (size_t i = 0; target[i] != '\0' && n + 2 < sizeof(model); i++) {
        if (i == 5) {
            model[n++] = '-';
        }
        model[n++] = (char)toupper((unsigned char)target[i]);
    }
    model[n] = '\0';

    printf("chip:     %s rev v%d.%d  cores=%d\n",
           model, info.revision / 100, info.revision % 100, info.cores);
    printf("features: %s%s%s%s\n",
           (info.features & CHIP_FEATURE_WIFI_BGN)   ? "Wi-Fi "    : "",
           (info.features & CHIP_FEATURE_BT)         ? "BT "       : "",
           (info.features & CHIP_FEATURE_BLE)        ? "BLE "      : "",
           (info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4 " : "");

    /* Sizes are read from the hardware: esp_chip_info()'s EMB_FLASH/EMB_PSRAM
     * flags are fixed per chip model and say nothing about this board. */
    uint32_t flash_size = 0;
    if (esp_flash_get_physical_size(NULL, &flash_size) == ESP_OK) {
        printf("flash:    %" PRIu32 " MB\n", flash_size / (1024 * 1024));
    } else {
        printf("flash:    unknown\n");
    }
#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        printf("psram:    %u MB\n", (unsigned)(esp_psram_get_size() / (1024 * 1024)));
    } else {
        printf("psram:    enabled but not detected\n");
    }
#else
    printf("psram:    not enabled in this build\n");
#endif
    printf("mac:      %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}

static void zc_con_chip_init(void)
{
    /* Zero-init + assign: newer IDF adds fields to esp_console_cmd_t and the
     * build runs with -Werror=missing-field-initializers. */
    esp_console_cmd_t con_chip_c = {};
    con_chip_c.command = "chip";
    con_chip_c.help = "Chip model, revision, features, MAC";
    con_chip_c.func = &con_chip_cmd;
    esp_console_cmd_register(&con_chip_c);
}

/* behaviors/console_version (CON_VER) */
static int con_ver_cmd(int argc, char **argv)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    printf("firmware:    %s\n", desc ? desc->project_name : "(unknown)");
    printf("version:     %s\n", desc ? desc->version : "(unknown)");
    printf("build date:  %s %s\n", desc ? desc->date : "?", desc ? desc->time : "?");
    printf("IDF version: %s\n", esp_get_idf_version());
    return 0;
}

static void zc_con_ver_init(void)
{
    /* Zero-init + assign: newer IDF adds fields to esp_console_cmd_t and the
     * build runs with -Werror=missing-field-initializers. */
    esp_console_cmd_t con_ver_c = {};
    con_ver_c.command = "version";
    con_ver_c.help = "Firmware + IDF version";
    con_ver_c.func = &con_ver_cmd;
    esp_console_cmd_register(&con_ver_c);
}

/* behaviors/console_log_level (CON_LOG) */
static int con_log_cmd(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: log <tag|*> <NONE|ERROR|WARN|INFO|DEBUG|VERBOSE>\n");
        return 1;
    }
    esp_log_level_t lvl;
    if      (!strcasecmp(argv[2], "NONE"))    lvl = ESP_LOG_NONE;
    else if (!strcasecmp(argv[2], "ERROR"))   lvl = ESP_LOG_ERROR;
    else if (!strcasecmp(argv[2], "WARN"))    lvl = ESP_LOG_WARN;
    else if (!strcasecmp(argv[2], "INFO"))    lvl = ESP_LOG_INFO;
    else if (!strcasecmp(argv[2], "DEBUG"))   lvl = ESP_LOG_DEBUG;
    else if (!strcasecmp(argv[2], "VERBOSE")) lvl = ESP_LOG_VERBOSE;
    else { printf("unknown level: %s\n", argv[2]); return 1; }
    esp_log_level_set(argv[1], lvl);
    printf("log level for '%s' set to %s\n", argv[1], argv[2]);
    return 0;
}

static void zc_con_log_init(void)
{
    /* Zero-init + assign: newer IDF adds fields to esp_console_cmd_t and the
     * build runs with -Werror=missing-field-initializers. */
    esp_console_cmd_t con_log_c = {};
    con_log_c.command = "log";
    con_log_c.help = "Set runtime log level for a tag";
    con_log_c.hint = "<tag|*> <NONE|ERROR|WARN|INFO|DEBUG|VERBOSE>";
    con_log_c.func = &con_log_cmd;
    esp_console_cmd_register(&con_log_c);
}

/* behaviors/console_heap_stats (CON_HEAP) */
static int con_heap_cmd(int argc, char **argv)
{
    const struct { uint32_t caps; const char *name; } regions[] = {
        { MALLOC_CAP_8BIT,    "8BIT (general)" },
        { MALLOC_CAP_INTERNAL,"INTERNAL (DRAM)" },
        { MALLOC_CAP_DMA,     "DMA-capable" },
        /* ESP-IDF 6 gates MALLOC_CAP_EXEC behind CONFIG_HEAP_HAS_EXEC_HEAP, so
           on a part with no executable heap (the C3, for one) the macro does
           not exist at all rather than naming an empty region. Reporting a row
           that cannot exist was never useful, so it is simply omitted there. */
#ifdef MALLOC_CAP_EXEC
        { MALLOC_CAP_EXEC,    "EXEC (IRAM)" },
#endif
    };
    printf("%-22s  %12s  %12s  %12s\n", "region", "free", "largest", "min_free");
    for (size_t i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        size_t f   = heap_caps_get_free_size(regions[i].caps);
        size_t lg  = heap_caps_get_largest_free_block(regions[i].caps);
        size_t mn  = heap_caps_get_minimum_free_size(regions[i].caps);
        printf("%-22s  %12u  %12u  %12u\n", regions[i].name,
               (unsigned)f, (unsigned)lg, (unsigned)mn);
    }
    return 0;
}

static void zc_con_heap_init(void)
{
    /* Zero-init + assign: newer IDF adds fields to esp_console_cmd_t and the
     * build runs with -Werror=missing-field-initializers. */
    esp_console_cmd_t con_heap_c = {};
    con_heap_c.command = "heap";
    con_heap_c.help = "Detailed heap stats per region";
    con_heap_c.func = &con_heap_cmd;
    esp_console_cmd_register(&con_heap_c);
}

void console_logic_init(void)
{
    zc_con_uptime_init();
    zc_con_chip_init();
    zc_con_ver_init();
    zc_con_log_init();
    zc_con_heap_init();
}
