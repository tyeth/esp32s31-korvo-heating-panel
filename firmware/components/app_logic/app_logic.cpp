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
#include "app_audio.h"
#include "app_console.h"
#include "app_driver.h"
#include "app_mqtt.h"
#include "app_agent.h"
#include "heating.h"

#include <strings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nvs.h>

#include <esp_log.h>
#include <esp_console.h>
#include <esp_err.h>

static void set_strip(uint8_t hue, uint8_t sat, uint8_t bri);
static void strip_off(void);
static void strip_idle(void);
static void agent_ops_install(void);

/* ── one console command, for bench use with no network ──────────────── */

static void heating_console_usage(void)
{
    printf("usage: heating set <room 0-%d> <target \xC2\xB0" "C, 5..30>\n", HEATING_ROOMS - 1);
    for (int i = 0; i < HEATING_ROOMS; i++) {
        heating_room_t *r = heating_room(i);
        int16_t t = heating_room_target_ui(r);
        printf("  %d  %-12s target %d.%d  state %s\n        setpoint -> %s\n", i, r->cfg.name,
               (int)(t / 10), (int)((t % 10 + 10) % 10), r->cfg.trv_topic,
               heating_room_command_topic(i));
    }
    /* What the panel thinks of its own link — the first thing to check when a
     * card sits empty or greyed. */
    printf("panel: Wi-Fi %s, broker session %s, %d/%d rooms reporting (%ds staleness)\n",
           app_mqtt_wifi_missing() ? "not configured" : "configured",
           app_mqtt_session_up() ? "up" : "down",
           heating_rooms_reporting(), HEATING_ROOMS, HEATING_STALE_S);
}

static int heating_console_cmd(int argc, char **argv);

static int mqtt_diag_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    heating_log_diag();
    return 0;
}

static int heating_console_cmd(int argc, char **argv)
{
    if (argc < 4 || strcmp(argv[1], "set") != 0) {
        heating_console_usage();
        return 1;
    }
    int idx = atoi(argv[2]);
    if (idx < 0 || idx >= HEATING_ROOMS) {
        printf("no room %d\n", idx);
        return 1;
    }

    /* "21" / "21.5": degrees with at most one decimal, like the settings. */
    char *end = NULL;
    double deg = strtod(argv[3], &end);
    if (end == argv[3] || (end != NULL && *end != '\0')) {
        printf("not a temperature: %s\n", argv[3]);
        return 1;
    }
    if (deg < HEATING_MIN_TENTH / 10.0 || deg > HEATING_MAX_TENTH / 10.0) {
        printf("out of range: %.1f \xC2\xB0" "C (5.0 to 30.0)\n", deg);
        return 1;
    }
    int16_t want = heating_clamp_tenth((int16_t)(deg * 10.0 + (deg < 0 ? -0.5 : 0.5)));

    switch (heating_request_target(idx, want)) {
        case HEATING_REQ_OK:
            printf("%s -> %d.%d\n", heating_room(idx)->cfg.name,
                   (int)(want / 10), (int)((want % 10 + 10) % 10));
            return 0;
        case HEATING_REQ_ALREADY_SET:
            printf("already set\n");
            return 1;
        default:
            printf("offline \xE2\x80\x94 nothing published (the panel has no broker session)\n");
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

    /* One command for the whole MQTT picture: what is configured, whether the
     * session is up, and every room's state/setpoint/sensor topic with what has
     * been heard on it. */
    esp_console_cmd_t d = {};
    d.command = "mqtt-diag";
    d.help = "Log the MQTT config, session and every room topic with its readings";
    d.func = &mqtt_diag_cmd;
    esp_console_cmd_register(&d);
}

static int help_console_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_console_print_commands();
    return 0;
}

static void zc_con_print_help_init(void)
{
    /* The console has no `help` of its own: every esp_console_new_repl_*()
     * blanks the command registry AFTER the blocks have registered theirs
     * (app_console_print_commands' note has the details), so the product
     * brings its own. */
    esp_console_cmd_t con_help_c = {};
    con_help_c.command = "help";
    con_help_c.help = "List every console command";
    con_help_c.func = &help_console_cmd;
    esp_console_cmd_register(&con_help_c);
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

/* Strip brightness is the household's, not the firmware's: the WS2812 sits on
 * a wall or a nightstand and even the old 90/254 lit a dark room. Stored in
 * NVS ("heating"/"sbri"), set from the settings page, default LOW
 * (HEATING_STRIP_BRI_DEFAULT, heating.h); 0 keeps the strip dark. The dim
 * (idle/connecting) level follows at a quarter of the day level. */
static uint8_t s_bri_day = HEATING_STRIP_BRI_DEFAULT;
#define BRI_DAY      (s_bri_day)
#define BRI_DIM      ((uint8_t)(s_bri_day >= 8 ? s_bri_day / 4 : (s_bri_day ? 2 : 0)))

uint8_t heating_strip_brightness(void)
{
    return s_bri_day;
}

/* The save and the refresh run on the esp_timer task: the voice agent calls
 * heating_strip_set_brightness() from a task whose STACK is in PSRAM, and an
 * NVS commit disables the flash cache mid-write — the first touch of that
 * PSRAM stack then resets the chip (logged as a bare POWERON, no backtrace).
 * The esp_timer task's stack is internal RAM, where flash writes are legal. */
static esp_timer_handle_t s_bri_timer = NULL;

static void bri_commit_cb(void *arg)
{
    (void)arg;
    nvs_handle_t h;
    if (nvs_open("heating", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "sbri", s_bri_day);
        nvs_commit(h);
        nvs_close(h);
    }
    strip_idle();                     /* apply to whatever state is showing */
}

void heating_strip_set_brightness(uint8_t bri)
{
    s_bri_day = bri;
    if (s_bri_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = bri_commit_cb,
            .name = "sbri",
        };
        if (esp_timer_create(&args, &s_bri_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_bri_timer);
    esp_timer_start_once(s_bri_timer, 1000);   /* now, but off this task */
}

static void strip_load_brightness(void)
{
    nvs_handle_t h;
    if (nvs_open("heating", NVS_READONLY, &h) == ESP_OK) {
        uint8_t b = 0;
        if (nvs_get_u8(h, "sbri", &b) == ESP_OK) s_bri_day = b;
        nvs_close(h);
    }
}

#define FR_HOLD_MS   10000  /* SET + MODE, the documented factory reset */
#define LISTEN_MS    10000

/* The listening window: opened by the button or the wake word, closed by a
 * recognised command, a second press, or ten seconds. */
static volatile bool s_listening = false;
static volatile bool s_refused = false;
static esp_timer_handle_t s_listen_timer = NULL;
static esp_timer_handle_t s_red_timer = NULL;

/* The first half of the SET+MODE combination: SET held this long arms the
 * factory-reset countdown (the countdown itself then runs FR_HOLD_MS). */
#define COMBO_ARM_MS 1000

/* Brought up without a working touch panel: there is no on-screen way in, so
 * the panel doubles as a device that must be set up over the console. */
static bool s_touchless = false;

/* The last time anything the dashboard mirrors changed (a reading, an
 * online/offline transition, a heard command, a lamp; a button press). Only
 * used to decide when the screen may dim. */
static volatile int64_t s_last_event_ms = 0;

/* ── status strip ────────────────────────────────────────────────────── */

static void set_strip(uint8_t hue, uint8_t sat, uint8_t bri)
{
    if (bri == 0) {                   /* brightness 0 = the strip stays dark */
        strip_off();
        return;
    }
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
    else if (s_touchless)
                                     set_strip(HUE_RED, 0, BRI_DIM);  /* console only */
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

/* The 10 s window ended with nothing recognised: the microphone closes too. */
static void listen_timeout_cb(void *arg)
{
    (void)arg;
    app_audio_listen_stop();
    heating_voice_not_understood();
}

/* heating_voice_on_event() lives in heating_voice.cpp with the rest of the
 * audio hooks. */

/* ── the front buttons ───────────────────────────────────────────────── */

typedef struct {
    button_handle_t set;
    bool set_down;      /* SET is held right now */
} combo_t;
static combo_t s_combo;
static esp_timer_handle_t s_fr_timer = NULL;

static void factory_reset_cb(void *arg)
{
    (void)arg;
    heating_ui_banner("Factory reset");
    ESP_LOGW(TAG, "front button held for %d ms — wiping settings", FR_HOLD_MS);
    set_strip(HUE_RED, SAT_MAX, BRI_DAY);
    heating_factory_reset();
}

static void on_set_double_click(void *arg, void *usr_data);
static void alert_silence(void);

/* The hold: nothing is armed by a press. The countdown starts only once the
 * button has actually been HELD for ARM_MS (a long-press event), and a real
 * release cancels it — a tap, a slipped finger or a lost release can never
 * schedule a wipe. Ten seconds of hold wipes the settings and reboots the
 * panel into onboarding: the spec's factory reset, on the one front button the
 * board's button device reports. */
static void on_set_press(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    heating_ui_touch_activity();
    s_combo.set_down = true;
}

static void on_set_arm(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    if (!s_combo.set_down) return;
    /* Held, not tapped: the countdown starts here and only here. */
    heating_ui_banner("Reset in 10 s - release to cancel");
    if (s_fr_timer) {
        esp_timer_stop(s_fr_timer);
        esp_timer_start_once(s_fr_timer, (uint64_t)FR_HOLD_MS * 1000);
    }
}

static void on_set_release(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    s_combo.set_down = false;
    /* Any release disarms — nothing can latch. */
    if (s_fr_timer) esp_timer_stop(s_fr_timer);
}

static void on_set_double_click(void *arg, void *usr_data)
{
    (void)arg; (void)usr_data;
    alert_silence();
}

/* Dismiss a banner or a running window that is only feedback; never opens the
 * microphone. */
static void alert_silence(void)
{
    heating_ui_touch_activity();
    if (s_listen_timer) esp_timer_stop(s_listen_timer);
    heating_ui_banner("");
}

/* One push-to-talk window: opens the mic for 10 s, a second press closes it.
 * Shared by SET and BOOT, the two documented triggers; the second press does
 * not merely hide the window — it takes the microphone away as well. */
static void push_to_talk(void)
{
    heating_ui_touch_activity();
    if (s_listening) {
        heating_ui_listening(false);
        app_audio_listen_stop();
        return;
    }
    app_audio_listen_once();
    heating_ui_listening(true);
    heating_ui_banner("Listening...");
    if (s_listen_timer) esp_timer_start_once(s_listen_timer, (uint64_t)LISTEN_MS * 1000);
}

/* drivers/board_button's long press on the group handle. */
void set_btn_on_long_press(void)
{
    alert_silence();
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
    if (heating_online()) {
        if (heating_permit_join() == ESP_OK) {
            heating_ui_banner("Permit join for 4 minutes sent");
            return;
        }
    }
    heating_ui_banner("Offline - controls disabled");
    heating_ui_rejected();
}

/**
 * The board's adc_button_group carries SET, MODE, VOL− and VOL+ on ONE
 * iot_button device: the button component reports the whole group as a single
 * button and only handles[0] is a source of events (see control.cpp). So the
 * front panel is driven entirely through that handle:
 *
 *   click        push-to-talk (spec's front SET button)
 *   double click dismiss the banner
 *   hold         SET kept down for ARM_MS (a long-press event) starts the
 *                countdown, and ten seconds of hold total performs the reset
 *                — a tap can never schedule one
 *
 * MODE's own events are never consulted (no release could clear a state they
 * set, which is exactly how a factory reset once armed itself). */
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

    s_combo.set = btns->button_handles[0];

    iot_button_register_cb(s_combo.set, BUTTON_SINGLE_CLICK, NULL, on_set_click, NULL);
    iot_button_register_cb(s_combo.set, BUTTON_DOUBLE_CLICK, NULL, on_set_double_click, NULL);

    /* The one hold that means anything: SET held for ten seconds. The press
     * and release bracket it (any release disarms); the countdown is armed by
     * the long-press event at ARM_MS, and the wipe is performed by that timer —
     * not by the event, so the two can never disagree. */
    iot_button_register_cb(s_combo.set, BUTTON_PRESS_DOWN, NULL, on_set_press, NULL);
    iot_button_register_cb(s_combo.set, BUTTON_PRESS_UP, NULL, on_set_release, NULL);
    button_event_args_t lp = {};
    lp.long_press.press_time = COMBO_ARM_MS;
    iot_button_register_cb(s_combo.set, BUTTON_LONG_PRESS_START, &lp, on_set_arm, NULL);

    ESP_LOGI(TAG, "front buttons bound (%d) — click = push-to-talk, double click = dismiss, "
                  "hold %d ms = factory reset", (int)btns->num_buttons, FR_HOLD_MS);
}

esp_err_t app_logic_init(void)
{
    /* heating_init() already ran in app_main (the model and the MQTT handler
     * come up before the frameworks that use them); this only re-reads the
     * saved targets so the cards start on them. */
    heating_load_targets();

    const esp_timer_create_args_t red_args = { .callback = &red_timer_cb, .name = "heating_red" };
    esp_timer_create(&red_args, &s_red_timer);
    const esp_timer_create_args_t listen_args = { .callback = &listen_timeout_cb, .name = "heating_listen" };
    esp_timer_create(&listen_args, &s_listen_timer);
    const esp_timer_create_args_t fr_args = { .callback = &factory_reset_cb, .name = "heating_fr" };
    esp_timer_create(&fr_args, &s_fr_timer);

    strip_load_brightness();
    bind_adc_buttons();
    heating_console_register();
    zc_con_print_help_init();
    heating_broker_ready(false);

    /* The panel may already have come up before this ran: point the strip at
     * what the panel actually offers, and start the dashboard's idle clock. */
    s_touchless = !heating_ui_has_touch();
    s_last_event_ms = esp_timer_get_time() / 1000;

    /* "Connecting" until the broker session is up (off before Wi-Fi exists). */
    strip_idle();

    agent_ops_install();

    ESP_LOGI(TAG, "heating panel logic up");
    return ESP_OK;
}

/* ── the voice agent's view of the product (app_agent_ops_t) ──────────── */

static int agent_set_target(int room_idx, float celsius)
{
    int16_t tenth = heating_clamp_tenth((int16_t)(celsius * 10.0f + (celsius >= 0 ? 0.5f : -0.5f)));
    switch (heating_request_target(room_idx, tenth)) {
        case HEATING_REQ_OK:          return 0;
        case HEATING_REQ_OFFLINE:     return 1;
        case HEATING_REQ_ALREADY_SET: return 2;
        default:                      return 3;
    }
}

static int agent_room_count(void) { return HEATING_ROOMS; }

static int agent_find_room(const char *name)
{
    if (name == NULL || name[0] == 0) return -1;
    for (int i = 0; i < HEATING_ROOMS; i++) {
        if (strcasecmp(heating_room(i)->cfg.name, name) == 0) return i;
    }
    /* Second pass, forgiving: "the office" or "office room" still lands. */
    for (int i = 0; i < HEATING_ROOMS; i++) {
        const char *rn = heating_room(i)->cfg.name;
        if (strcasestr(name, rn) != NULL || strcasestr(rn, name) != NULL) return i;
    }
    return -1;
}

static const char *agent_room_name(int idx)
{
    heating_room_t *r = heating_room(idx);
    return r ? r->cfg.name : "";
}

static void agent_status_json(char *buf, size_t len)
{
    size_t w = (size_t)snprintf(buf, len, "{\"online\":%s,\"rooms\":[",
                                heating_online() ? "true" : "false");
    for (int i = 0; i < HEATING_ROOMS && w + 8 < len; i++) {
        heating_room_t *r = heating_room(i);
        int16_t tenth = 0;
        bool stale = true;
        bool have = heating_room_current(r, &tenth, &stale, NULL);
        if (have) {
            w += (size_t)snprintf(buf + w, len - w,
                     "%s{\"name\":\"%s\",\"current_c\":%.1f,\"target_c\":%.1f,\"fresh\":%s}",
                     i ? "," : "", r->cfg.name, tenth / 10.0,
                     heating_room_target_ui(r) / 10.0, stale ? "false" : "true");
        } else {
            w += (size_t)snprintf(buf + w, len - w,
                     "%s{\"name\":\"%s\",\"current_c\":null,\"target_c\":%.1f,\"fresh\":false}",
                     i ? "," : "", r->cfg.name, heating_room_target_ui(r) / 10.0);
        }
    }
    if (w + 3 < len) snprintf(buf + w, len - w, "]}");
}

static void agent_banner(const char *text)
{
    heating_ui_banner(text);
    heating_ui_poke();
}

static void agent_pause_audio(bool pause)
{
    app_audio_pause_feed(pause);
}

static int agent_set_strip_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint8_t bri = (uint8_t)((percent * 254 + 50) / 100);
    heating_strip_set_brightness(bri);
    return bri;
}

static void agent_ops_install(void)
{
    static const app_agent_ops_t k_ops = {
        agent_set_target, agent_room_count, agent_find_room,
        agent_room_name, agent_status_json, agent_banner,
        agent_set_strip_percent, agent_pause_audio,
    };
    app_agent_set_ops(&k_ops);
    app_agent_init();
}
