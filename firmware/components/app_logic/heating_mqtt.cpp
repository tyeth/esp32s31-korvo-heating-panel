/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The product's half of the MQTT framework: its own handler installed on the
 * framework's single session, and the install call app_main makes before
 * app_mqtt_init() (the handler must be in place before the first message can
 * arrive).
 */

#include "heating.h"
#include "app_mqtt.h"

#include <esp_log.h>

static const char *TAG = "heating_mqtt";

/* esp-mqtt hands the payload over on its own task, not NUL-terminated. */
static void on_message(const char *topic, int topic_len, const char *data, int data_len)
{
    heating_on_mqtt_message(topic, topic_len, data, data_len);
}

void heating_mqtt_install(void)
{
    app_mqtt_set_data_handler(on_message);
    ESP_LOGI(TAG, "zigbee2mqtt state handler installed on the MQTT session");
}
