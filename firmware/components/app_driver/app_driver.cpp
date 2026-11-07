#include "app_driver.h"
#include "app_config.h"

#include <esp_log.h>


static const char *TAG = "app_driver";





esp_err_t app_driver_init(void)
{


    ESP_LOGI(TAG, "Hardware drivers initialized");
    return ESP_OK;
}

esp_err_t app_driver_apply_param(app_driver_param_id_t id, app_driver_param_val_t val)
{
    switch (id) {

        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}
