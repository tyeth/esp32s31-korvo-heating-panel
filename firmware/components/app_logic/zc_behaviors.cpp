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
/** Start the SNTP client on its own task, once the default event loop exists. */
void diagnostics_sntp_start(void);

esp_err_t zc_behaviors_init(void)
{
    control_logic_init();
    diagnostics_logic_init();
    console_logic_init();

    ESP_LOGI(TAG, "Composed behaviors initialized");
    return ESP_OK;
}
