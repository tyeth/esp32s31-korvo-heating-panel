/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The panel's own additions on top of the composed blocks: the front-button
 * gestures, the factory-reset combination and the status strip.
 *
 * The status strip is the board's own WS2812, driven through the colour params
 * the board_led_strip block owns:
 *   dim white while connecting · green online · amber listening ·
 *   red for a second on a refused command.
 */

#include "app_logic.h"
#include "app_config.h"
#include "app_driver.h"
#include "app_mqtt.h"
#include "heating.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_console.h>
#include <esp_err.h>

static void set_strip(uint8_t hue, uint8_t sat, uint8_t bri);
static void strip_idle(void);

/* ── one console command, for bench use with no network ──────────────── */

static int heating_console_cmd(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "set") != 0) {
        printf("usage: heating set <room 0-%d> <target \xC2\xB0" "C, 5..30>\n", HEATING_ROOMS - 1);
        for (int i = 0; i < HEATING_ROOMS; i++) {
            heating_room_t *r = heating_room(i);
            int16_t t = heating_room_target_ui(r);
            printf("  %d  %-12s target %d.%d  %s\n", i, r->cfg.name,
                   (int)(t / 10), (int)((t % 10 + 10) % 10), r->cfg.trv_topic);
        }
        return 1;
    }
    int idx = atoi(argv[2]);
    if (idx < 0 || idx >= HEATING_ROOMS) {
        printf("no room %d\n", idx);
        return 1;
    }
    int whole = atoi(argv[3]);
    int tenth = whole * 10;
    switch (heating_request_target(idx, (int16_t)tenth)) {
        case HEATING_REQ_OK:
            printf("%s -> %d.0\n", heating_room(idx)->cfg.name, whole);
            return 0;
        case HEATING_REQ_ALREADY_SET:
            printf("already set\n");
            return 1;
        default:
            printf("offline \xE2\x80\x94 nothing published\n");
            return 1;
    }
}

void heating_console_register(void)
{
    esp_console_cmd_t c = {};
    c.command = "heating";
    c.help = "Show the rooms / set a target: heating set <room> <\xC2\xB0" "C>";
    c.func = &heating_console_cmd;
    esp_console_cmd_register(&c);
}

/* ── panel init ──────────────────────────────────────────────────────── */

#include <esp_system.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/gpio.h>
#include <iot_button.h>
#include <esp_board_manager.h>
#include <dev_button.h>

static const char *TAG = "heating_panel";

/* Matter's 0..254 hue scale. */
#define HUE_GREEN    66
#define HUE_AMBER    28
#define HUE_RED      0
#define SAT_MAX      254
#define BRI_DAY      90     /* kept low: it sits on a wall, or a nightstand */
#define BRI_DIM      16

#define FR_HOLD_MS   10000  /* SET + MODE, the documented factory reset */
#define LISTEN_MS    10000

/* The listening window: opened by the button or the wake word, closed by a
 * recognised command, a second press, or ten seconds. */
static volatile bool s_listening = false;
static volatile bool s_refused = false;
static esp_timer_handle_t s_listen_timer = NULL;
static esp_timer_handle_t s_red_timer = NULL;

/* ── status strip ────────────────────────────────────────────────────── */

static void set_strip(uint8_t hue, uint8_t sat, uint8_t bri)
{
    app_driver_param_val_t v = {};
    v.u8 = hue; app_driver_set_param(APP_DRIVER_PARAM_HUE, v, APP_DRIVER_SOURCE_LOCAL);
    v.u8 = sat; app_driver_set_param(APP_DRIVER_PARAM_SATURATION, v, APP_DRIVER_SOURCE_LOCAL);
    v.u8 = bri; app_driver_set_param(APP_DRIVER_PARAM_BRIGHTNESS, v, APP_DRIVER_SOURCE_LOCAL);
    v.b = true; app_driver_set_param(APP_DRIVER_PARAM_POWER, v, APP_DRIVER_SOURCE_LOCAL);
}

static void strip_off(void)
{
    app_driver_param_val_t v = {};
    v.b = false;
    app_driver_set_param(APP_DRIVER_PARAM_POWER, v, APP_DRIVER_SOURCE_LOCAL);
}

/* The spec's four states, in priority order. A refused command while offline
 * flashes red for its second and then returns to the idle colour (dim white,
 * still connecting) — never green, and never a POWER off/on cycle per tap. */
static void strip_idle(void)
{
    if (s_listening)                 set_strip(HUE_AMBER, SAT_MAX, BRI_DAY);
    else if (s_refused)              set_strip(HUE_RED,   SAT_MAX, BRI_DAY);
    else if (heating_online())       set_strip(HUE_GREEN, SAT_MAX, BRI_DAY);
    else if (app_mqtt_configured())  set_strip(0, 0, BRI_DIM);   /* connecting */
    else                             strip_off();                /* not set up */
}

void heating_ui_sync_online(void)
{
    /* Re-evaluated on every broker transition, not only on a rejection. */
    strip_idle();
}

/* Until the Wi-Fi radio is up there is nothing to connect to: strip off. */
void heating_web_announce(void)
{
    strip_idle();
}

static void red_timer_cb(void *arg)
{
    (void)arg;
    s_refused = false;
    strip_idle();
}

void heating_ui_rejected(void)
{
    s_refused = true;
    strip_idle();
    heating_ui_rejected_ui();
    /* red for one second, then back to the idle colour */
    if (s_red_timer) {
        esp_timer_stop(s_red_timer);
        esp_timer_start_once(s_red_timer, 1000 * 1000);
    }
}

/* ── voice ───────────────────────────────────────────────────────────── */

void heating_ui_listening(bool listening)
{
    bool was = s_listening;
    s_listening = listening;
    strip_idle();
    heating_ui_listening_ui(listening);
    if (was && !listening && s_listen_timer) {
        /* The window closed (a recognised command, a second press or the 10 s
         * timeout) — nothing else keeps the countdown running. */
        esp_timer_stop(s_listen_timer);
    }
}

/* Called when the 10 s window ends with no recognised command. */
static void listen_timeout_cb(void *arg)
{
    (void)arg;
    heating_voice_not_understood();
}

/* heating_voice_on_event() lives in heating_voice.cpp with the rest of the
 * audio hooks. */

/* ── the front buttons ───────────────────────────────────────────────── */

typedef struct {
    button_handle_t set;
    button_handle_t mode;
    bool set_down;
    bool mode_down;
} combo_t;
static combo_t s_combo;
static esp_timer_handle_t s_fr_timer = NULL;

static void factory_reset_cb(void *arg)
{
    (void)arg;
    heating_ui_banner("Factory reset");
    ESP_LOGW(TAG, "SET + MODE held for %d ms — wiping settings", FR_HOLD_MS);
    set_strip(HUE_RED, SAT_MAX, BRI_DAY);
    heating_factory_reset();
}

/* SET + MODE held together: the countdown banner runs for 10 s, then the
 * settings go and the panel reboots into onboarding. */
static void combo_eval(void)
{
    bool both = s_combo.set_down && s_combo.mode_down;
    if (!both) {
        if (s_fr_timer) esp_timer_stop(s_fr_timer);
        return;
    }
    heating_ui_banner("Factory reset in 10 s — release to cancel");
    if (s_fr_timer) {
        esp_timer_stop(s_fr_timer);
        esp_timer_start_once(s_fr_timer, (uint64_t)FR_HOLD_MS * 1000);
    }
}

static void on_set_press(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    heating_ui_touch_activity();
    s_combo.set_down = true;
    combo_eval();
}

static void on_set_release(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    s_combo.set_down = false;
    combo_eval();
}

static void on_mode_press(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    s_combo.mode_down = true;
    combo_eval();
}

static void on_mode_release(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    s_combo.mode_down = false;
    combo_eval();
}

/* One push-to-talk window: opens the mic for 10 s, a second press closes it.
 * Shared by SET and BOOT, the two documented triggers. */
static void push_to_talk(void)
{
    heating_ui_touch_activity();
    if (s_listening) {
        heating_ui_listening(false);
        return;
    }
    heating_ui_listening(true);
    heating_ui_banner("Listening\xE2\x80\xA6");
    if (s_listen_timer) esp_timer_start_once(s_listen_timer, (uint64_t)LISTEN_MS * 1000);
}

static void on_set_click(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    /* A click that was part of the SET+MODE hold never reaches here — the
     * reset has restarted the panel by then. */
    push_to_talk();
}

/* Push-to-talk on the board's boot button: one press, one listening window. */
void boot_btn_on_single_click(void)
{
    push_to_talk();
}

/* Long press on the boot button: no Wi-Fi stored yet means there is no link
 * to publish on, so it (re)opens the onboarding access point instead; on a
 * configured panel it asks zigbee2mqtt to permit a device. */
void boot_btn_on_long_press(void)
{
    heating_ui_touch_activity();
    if (app_mqtt_wifi_missing()) {
        app_mqtt_ap_start();
        char ssid[33];
        app_mqtt_ap_ssid(ssid, sizeof(ssid));
        char msg[64];
        snprintf(msg, sizeof(msg), "Join Wi-Fi \"%s\" for settings", ssid);
        heating_ui_banner(msg);
        return;
    }
    if (heating_permit_join() == ESP_OK) {
        heating_ui_banner("Permit join for 4 minutes sent");
    } else {
        heating_ui_banner("Offline — controls disabled");
        heating_ui_rejected();
    }
}

/**
 * The board's adc_button_group is one device carrying SET, MODE, VOL− and
 * VOL+, reported as four button handles in that order. SET and MODE are the
 * factory-reset combination; any of the four wakes the screen.
 */
static void bind_adc_buttons(void)
{
    void *h = NULL;
    if (esp_board_manager_get_device_handle("adc_button_group", &h) != ESP_OK || h == NULL) {
        ESP_LOGW(TAG, "board device 'adc_button_group' has no handle — front buttons disabled");
        return;
    }
    dev_button_handles_t *btns = (dev_button_handles_t *)h;
    if (btns->num_buttons < 1 || btns->button_handles[0] == NULL) {
        ESP_LOGW(TAG, "board device 'adc_button_group' carries no buttons");
        return;
    }

    s_combo.set  = btns->button_handles[0];
    s_combo.mode = btns->num_buttons > 1 ? btns->button_handles[1] : NULL;

    iot_button_register_cb(s_combo.set, BUTTON_PRESS_DOWN, NULL, on_set_press, NULL);
    iot_button_register_cb(s_combo.set, BUTTON_PRESS_UP, NULL, on_set_release, NULL);
    /* SET is the documented push-to-talk button: one click opens the mic, the
     * 10 s window closes it again. MODE keeps its own press/release so the
     * two held together still reach the factory-reset countdown. */
    iot_button_register_cb(s_combo.set, BUTTON_SINGLE_CLICK, NULL, on_set_click, NULL);
    if (s_combo.mode) {
        iot_button_register_cb(s_combo.mode, BUTTON_PRESS_DOWN, NULL, on_mode_press, NULL);
        iot_button_register_cb(s_combo.mode, BUTTON_PRESS_UP, NULL, on_mode_release, NULL);
    }
    ESP_LOGI(TAG, "front buttons bound (%d) — SET = push-to-talk, SET+MODE %d ms = factory reset",
             (int)btns->num_buttons, FR_HOLD_MS);
}

esp_err_t app_logic_init(void)
{
    heating_init();

    const esp_timer_create_args_t red_args = { .callback = &red_timer_cb, .name = "heating_red" };
    esp_timer_create(&red_args, &s_red_timer);
    const esp_timer_create_args_t listen_args = { .callback = &listen_timeout_cb, .name = "heating_listen" };
    esp_timer_create(&listen_args, &s_listen_timer);
    const esp_timer_create_args_t fr_args = { .callback = &factory_reset_cb, .name = "heating_fr" };
    esp_timer_create(&fr_args, &s_fr_timer);

    bind_adc_buttons();
    heating_console_register();
    heating_broker_ready(false);

    /* "Connecting" until the broker session is up (off before Wi-Fi exists). */
    strip_idle();

    ESP_LOGI(TAG, "heating panel logic up");
    return ESP_OK;
}
