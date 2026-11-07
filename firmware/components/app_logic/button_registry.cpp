#include "app_button_registry.h"
#include <esp_log.h>

static const char *TAG = "button_registry";

#define APP_BUTTON_REGISTRY_MAX 8

typedef struct {
    int gpio_num;
    button_handle_t handle;
} app_button_entry_t;

static app_button_entry_t s_buttons[APP_BUTTON_REGISTRY_MAX];
static int s_button_count = 0;

button_handle_t app_button_get_or_create(int gpio_num, int active_level, int long_press_ms)
{
    for (int i = 0; i < s_button_count; i++) {
        if (s_buttons[i].gpio_num == gpio_num) {
            return s_buttons[i].handle;
        }
    }
    if (s_button_count >= APP_BUTTON_REGISTRY_MAX) {
        ESP_LOGE(TAG, "button registry full (max %d) — GPIO %d not created", APP_BUTTON_REGISTRY_MAX, gpio_num);
        return NULL;
    }

    button_config_t btn_cfg = {};
    btn_cfg.long_press_time = (uint16_t)long_press_ms;
    button_gpio_config_t gpio_cfg = {
        .gpio_num = gpio_num,
        .active_level = (uint8_t)active_level,
        .enable_power_save = false,
        .disable_pull = false,
    };
    button_handle_t handle = NULL;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &handle);
    if (err != ESP_OK || !handle) {
        ESP_LOGE(TAG, "failed to create button on GPIO %d: %s", gpio_num, esp_err_to_name(err));
        return NULL;
    }
    s_buttons[s_button_count].gpio_num = gpio_num;
    s_buttons[s_button_count].handle = handle;
    s_button_count++;
    ESP_LOGI(TAG, "created shared button device on GPIO %d", gpio_num);
    return handle;
}
