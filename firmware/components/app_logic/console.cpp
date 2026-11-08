#include "app_logic.h"
#include "app_console.h"
#include "app_driver.h"
#include "app_config.h"
#include "heating.h"
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

/* behaviors/console_log_level (CON_LOG)
 *
 * `log` on its own lists the tags this panel logs under and where each one
 * currently stands (the stored verbosity, the noisy tags held below it, and a
 * tag named here that has since changed). `log <tag> [LEVEL]` reads one tag,
 * `log <tag> <LEVEL>` overrides it, and `log * <LEVEL>` is the panel-wide
 * choice — the same one the settings page offers, saved so it survives a
 * reboot. */
static const struct { const char *name; esp_log_level_t level; } k_levels[] = {
    { "NONE",    ESP_LOG_NONE    },
    { "ERROR",   ESP_LOG_ERROR   },
    { "WARN",    ESP_LOG_WARN    },
    { "INFO",    ESP_LOG_INFO    },
    { "DEBUG",   ESP_LOG_DEBUG   },
    { "VERBOSE", ESP_LOG_VERBOSE },
};

static const char *level_name(esp_log_level_t lvl)
{
    for (size_t i = 0; i < sizeof(k_levels) / sizeof(k_levels[0]); i++) {
        if (k_levels[i].level == lvl) return k_levels[i].name;
    }
    return "?";
}

static bool level_parse(const char *s, esp_log_level_t *out)
{
    for (size_t i = 0; i < sizeof(k_levels) / sizeof(k_levels[0]); i++) {
        if (strcasecmp(s, k_levels[i].name) == 0) {
            *out = k_levels[i].level;
            return true;
        }
    }
    return false;
}

/* Every tag this panel writes under, grouped the way a reader looks for them:
 * the product's own, the framework components, then the driver tags that are
 * held quiet. A tag added to the firmware belongs in this table. */
static const struct { const char *name; const char *what; } k_tags[] = {
    { "app_main",       "boot sequence"                       },
    { "heating",        "rooms, topics, setpoints"           },
    { "heating_ui",     "the LCD dashboard"                   },
    { "heating_web",    "the /settings page"                  },
    { "heating_voice",  "microphone and spoken commands"      },
    { "heating_mqtt",   "the product's MQTT handler"          },
    { "heating_panel",  "buttons, status strip, console cmds" },
    { "heating_log",    "console verbosity"                   },
    { "diagnostics",    "boot reason, clock, status line"      },
    { "control",        "the board buttons"                   },
    { "button_registry","button handle registry"              },
    { "zc_behaviors",   "the composed behaviors' entry point" },
    { "app_audio",      "codec, microphone, recognition"      },
    { "app_display",    "panel, touch, LVGL"                  },
    { "app_driver",     "the board's device handles"           },
    { "app_driver_cb",  "driver callbacks"                    },
    { "app_mqtt",       "Wi-Fi, broker, MQTT session"         },
    { "app_console",    "the console REPL"                    },
    { "app_webui",      "the HTTP server"                     },
    { "MSPI",           "SPI-mem driver — held at WARN"        },
    { "ADC",            "ADC oneshot driver — held at WARN"    },
};

static void log_list(void)
{
    printf("console verbosity: %s%s\n", heating_log_level_get(),
           strcmp(heating_log_level_get(), "VERBOSE") == 0
               ? "  (everything; 'log * INFO' quiets it)" : "");
    printf("  %-16s %-8s %s\n", "tag", "level", "what it logs");
    for (size_t i = 0; i < sizeof(k_tags) / sizeof(k_tags[0]); i++) {
        esp_log_level_t lvl = esp_log_level_get(k_tags[i].name);
        printf("  %-16s %-8s %s\n", k_tags[i].name, level_name(lvl), k_tags[i].what);
    }
    printf("every other tag (esp-mqtt, Wi-Fi, TLS, LVGL, the board manager …) follows the "
           "console verbosity above.\n");
    printf("usage: log | log <tag> | log <tag|*> <NONE|ERROR|WARN|INFO|DEBUG|VERBOSE>\n");
}

static int con_log_cmd(int argc, char **argv)
{
    if (argc == 1) {
        log_list();
        return 0;
    }
    if (argc == 2) {
        printf("%-16s %s\n", argv[1], level_name(esp_log_level_get(argv[1])));
        return 0;
    }
    if (argc != 3) {
        printf("usage: log | log <tag> | log <tag|*> <NONE|ERROR|WARN|INFO|DEBUG|VERBOSE>\n");
        return 1;
    }
    esp_log_level_t lvl;
    if (!level_parse(argv[2], &lvl)) {
        printf("unknown level: %s (NONE, ERROR, WARN, INFO, DEBUG, VERBOSE)\n", argv[2]);
        return 1;
    }
    if (strcmp(argv[1], "*") == 0) {
        /* The panel-wide choice: stored, so it is still in force after a
         * reboot — the same setting the settings page shows. */
        if (heating_log_level_set(argv[2]) != ESP_OK) {
            printf("level not saved (NVS)\n");
            return 1;
        }
        printf("console verbosity %s (%s), saved\n", level_name(lvl), argv[2]);
        return 0;
    }
    esp_log_level_set(argv[1], lvl);
    printf("log level for '%s' set to %s (this boot only)\n", argv[1], argv[2]);
    return 0;
}

static void zc_con_log_init(void)
{
    /* Zero-init + assign: newer IDF adds fields to esp_console_cmd_t and the
     * build runs with -Werror=missing-field-initializers. */
    esp_console_cmd_t con_log_c = {};
    con_log_c.command = "log";
    con_log_c.help = "List the log tags and their levels; set one: log <tag|*> <LEVEL>";
    con_log_c.hint = "[<tag|*>] [NONE|ERROR|WARN|INFO|DEBUG|VERBOSE]";
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
