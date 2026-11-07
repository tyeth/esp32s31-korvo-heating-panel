#pragma once
/* Shared GPIO-button registry — keeps one iot_button device per GPIO so that
 * multiple consumers (e.g. a control button + long-press factory reset) on the
 * same pin share a single device and each register their own event callback. */
#include <iot_button.h>
#include <button_gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Get the iot_button handle for gpio_num, creating the device on first use.
 * active_level and long_press_ms are applied only when the device is first
 * created; later callers for the same GPIO get the existing handle. Pass
 * long_press_ms = 0 to leave the device-level long-press at its default.
 * Returns NULL on failure. */
button_handle_t app_button_get_or_create(int gpio_num, int active_level, int long_press_ms);

#ifdef __cplusplus
}
#endif
