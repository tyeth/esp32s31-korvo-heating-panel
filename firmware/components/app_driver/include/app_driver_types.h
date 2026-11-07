/* Param IDs: each driver/endpoint declares which it references; their
 * union forms this enum. APP_DRIVER_PARAM_MAX is the count for the state array. */
#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    APP_DRIVER_PARAM_NONE = 0,  /* sentinel */
    APP_DRIVER_PARAM_TONE_HZ,  /* uint32_t (union member: .u32) */
    APP_DRIVER_PARAM_VOLUME,  /* uint8_t (union member: .u8) */
    APP_DRIVER_PARAM_MUTE,  /* bool: on/off (union member: .b) */
    APP_DRIVER_PARAM_POWER,  /* bool: on/off (union member: .b) */
    APP_DRIVER_PARAM_HUE,  /* uint8_t (union member: .u8) */
    APP_DRIVER_PARAM_SATURATION,  /* uint8_t (union member: .u8) */
    APP_DRIVER_PARAM_BRIGHTNESS,  /* uint8_t (union member: .u8) */
    APP_DRIVER_PARAM_MAX
} app_driver_param_id_t;

typedef union {
    bool     b;
    uint8_t  u8;
    int16_t  i16;
    uint16_t u16;
    uint32_t u32;
} app_driver_param_val_t;

#define APP_DRIVER_MAX_SOLUTIONS 9  /* registrants in this composition + headroom */
typedef uint8_t app_driver_handle_t;
#define APP_DRIVER_SOURCE_LOCAL 0

typedef void (*app_driver_notify_cb_t)(
    app_driver_param_id_t param_id,
    app_driver_param_val_t val,
    app_driver_handle_t source,
    void *ctx);
