#include "app_logic.h"
#include "app_driver.h"
#include "app_config.h"
#include <esp_log.h>

static const char *TAG = "app_logic";

/* Product logic: state machines, automation rules, timers — whatever the
 * product does beyond what its composed blocks already do. Runs after the
 * drivers, the frameworks and the composed behaviors (zc_behaviors.cpp) are
 * up, so every bus param is readable and every solution is registered. */
esp_err_t app_logic_init(void)
{
    ESP_LOGI(TAG, "App logic initialized");
    return ESP_OK;
}
