/* GENERATED — the composed behaviors' entry point. Do not put product logic
 * here and do not edit: the generator rewrites this file. Product logic
 * belongs in app_logic.cpp (app_logic_init) or its own file. */
#include "app_logic.h"
#include <esp_log.h>

static const char *TAG = "zc_behaviors";

/* Each concern lives in its own translation unit — declared here, run below. */
void control_logic_init(void);
void diagnostics_logic_init(void);
void console_logic_init(void);
/** The SNTP clock, started once the network stack is up (from app_main). */
void diagnostics_clock_init(void);

/* The behaviours are diagnostics and helpers: the button bindings, the boot
 * reason / count, the watchdog log, the console commands. None of them is
 * essential to drawing the dashboard or talking to the broker, so a failure in
 * one is logged and the panel carries on instead of being aborted by app_main's
 * ESP_ERROR_CHECK. */
esp_err_t zc_behaviors_init(void)
{
    control_logic_init();
    diagnostics_logic_init();
    console_logic_init();

    ESP_LOGI(TAG, "Composed behaviors initialized");
    return ESP_OK;
}
