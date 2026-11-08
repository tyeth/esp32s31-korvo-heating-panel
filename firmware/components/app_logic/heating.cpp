/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The heating model: the four rooms and their zigbee2mqtt topics, what the TRVs
 * and the air sensors last reported, and the setpoint path out to the broker.
 *
 * One number is displayed everywhere — the target the TRV itself reports back.
 * A setpoint this panel publishes is only remembered (for the duplicate guard),
 * never shown as if it had been applied; the card follows the TRV's next state
 * message.
 */

#include "heating.h"
#include "app_config.h"
#include "app_mqtt.h"
#include "app_driver.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>

static const char *TAG = "heating";

/* The owner's four valves, pre-filled so re-pairing a room is a topic edit. */
static const heating_room_cfg_t k_defaults[HEATING_ROOMS] = {
    { "Bathroom",    "zigbee2mqtt/0xa4c1383b5b3d7063", "" },
    { "Hallway",     "zigbee2mqtt/0xa4c13852a667eedf", "" },
    { "Office",      "zigbee2mqtt/0xa4c13829e3ee602b", "" },
    { "Living room", "zigbee2mqtt/0xa4c13829aa5a7009", "" },
};

#define HEATING_NVS_NS "heating"

/* The nvs keys, one per stored field. */
static const char *const k_key_name[HEATING_ROOMS] = { "r0name", "r1name", "r2name", "r3name" };
static const char *const k_key_trv[HEATING_ROOMS]  = { "r0trv",  "r1trv",  "r2trv",  "r3trv"  };
static const char *const k_key_sensor[HEATING_ROOMS] = { "r0snr", "r1snr", "r2snr", "r3snr" };
static const char *const k_key_last[HEATING_ROOMS] = { "r0last", "r1last", "r2last", "r3last" };

/* Room names other than the owner's four, so "set the kitchen to 21" can be
 * answered with "Kitchen has no smart TRV" instead of silence. */
static const char *const k_unheated_names[] = { "Kitchen", "Bedroom" };

static heating_room_t s_rooms[HEATING_ROOMS];
static volatile bool s_online = false;

/* ── small helpers ───────────────────────────────────────────────────── */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void copy_str(char *dst, size_t len, const char *src)
{
    if (len == 0) return;
    strncpy(dst, src ? src : "", len - 1);
    dst[len - 1] = '\0';
}

/* Decimal degrees as a tenth-of-a-degree integer: "21" -> 210,
 * "21.5" -> 215, "21,5" -> 215. Returns false when the text is not a number. */
static bool parse_tenth(const char *s, const char *end, int16_t *out)
{
    while (s < end && (*s == ' ' || *s == '"')) s++;
    bool neg = false;
    if (s < end && (*s == '-' || *s == '+')) { neg = (*s == '-'); s++; }
    long whole = 0;
    bool any = false;
    while (s < end && *s >= '0' && *s <= '9') { whole = whole * 10 + (*s - '0'); s++; any = true; }
    long frac = 0;
    if (s < end && (*s == '.' || *s == ',')) {
        s++;
        int digits = 0;
        while (s < end && *s >= '0' && *s <= '9' && digits < 3) { frac = frac * 10 + (*s - '0'); s++; digits++; }
        any = true;
    }
    if (!any) return false;
    long tenth = whole * 10 + frac;          /* at most one displayed decimal */
    if (neg) tenth = -tenth;
    if (tenth < -3276 || tenth > 3276) return false;
    *out = (int16_t)tenth;
    return true;
}

/* Read one JSON value by key. Handles a number, a quoted string and the bare
 * literals; stops at the end of the value. */
static bool json_get(const char *json, int len, const char *key, char *out, size_t out_len)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    size_t pl = strlen(pat);
    for (int i = 0; i + (int)pl < len; i++) {
        if (memcmp(json + i, pat, pl) != 0) continue;
        int j = i + (int)pl;
        while (j < len && (json[j] == ' ' || json[j] == ':')) j++;
        if (j >= len) return false;
        if (json[j] == '"') {
            j++;
            int start = j;
            while (j < len && json[j] != '"') j++;
            size_t n = (size_t)(j - start);
            if (n >= out_len) n = out_len - 1;
            memcpy(out, json + start, n);
            out[n] = '\0';
            return true;
        }
        int start = j;
        while (j < len && json[j] != ',' && json[j] != '}' && json[j] != ' ') j++;
        size_t n = (size_t)(j - start);
        if (n >= out_len) n = out_len - 1;
        memcpy(out, json + start, n);
        out[n] = '\0';
        return true;
    }
    return false;
}

/* ── model ───────────────────────────────────────────────────────────── */

/* A room's zigbee2mqtt command topic — the topic that ACCEPTS a setpoint.
 *
 * The household's request was explicit: the state topics are right, but a
 * setpoint must go to the per-attribute command topic rather than back into the
 * state JSON. zigbee2mqtt exposes both forms:
 *   <device>/set/current_heating_setpoint   {"current_heating_setpoint": "21"}
 *   <device>/set                            {"current_heating_setpoint": "21"}
 * The per-attribute form is used when the room's stored topic is a plain state
 * topic; a topic that already names a /set/ (or /get) path is used as given, so
 * a household that prefers the aggregate form can store it on the settings page
 * without a rebuild. */
static void command_topic_for(const char *stored, char *out, size_t len)
{
    if (stored == NULL || stored[0] == 0) {
        out[0] = '\0';
        return;
    }
    if (strstr(stored, "/set") != NULL || strstr(stored, "/get") != NULL) {
        snprintf(out, len, "%s", stored);          /* already a command topic */
        return;
    }
    snprintf(out, len, "%s/set/current_heating_setpoint", stored);
}

const char *heating_room_command_topic(int idx)
{
    static char s_cmd[HEATING_ROOMS][HEATING_TOPIC_MAX + 32];
    if (idx < 0 || idx >= HEATING_ROOMS) return "";
    command_topic_for(s_rooms[idx].cfg.trv_topic, s_cmd[idx], sizeof(s_cmd[idx]));
    return s_cmd[idx];
}

heating_room_t *heating_room(int idx)
{
    if (idx < 0 || idx >= HEATING_ROOMS) return NULL;
    return &s_rooms[idx];
}

esp_err_t heating_init(void)
{
    for (int i = 0; i < HEATING_ROOMS; i++) {
        memset(&s_rooms[i], 0, sizeof(s_rooms[i]));
        s_rooms[i].cfg = k_defaults[i];
        /* Until a TRV reports, the card starts on the last setpoint this panel
         * sent (feature f13: the saved targets come up straight away). */
        s_rooms[i].read_target = 200;
        s_rooms[i].last_pub_tenth = 200;
    }

    nvs_handle_t h;
    if (nvs_open(HEATING_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved room table — using the defaults");
        return ESP_OK;
    }
    for (int i = 0; i < HEATING_ROOMS; i++) {
        size_t n;
        n = sizeof(s_rooms[i].cfg.name);         nvs_get_str(h, k_key_name[i],   s_rooms[i].cfg.name, &n);
        n = sizeof(s_rooms[i].cfg.trv_topic);    nvs_get_str(h, k_key_trv[i],    s_rooms[i].cfg.trv_topic, &n);
        n = sizeof(s_rooms[i].cfg.sensor_topic); nvs_get_str(h, k_key_sensor[i], s_rooms[i].cfg.sensor_topic, &n);
        int16_t last = 0;
        if (nvs_get_i16(h, k_key_last[i], &last) == ESP_OK) {
            s_rooms[i].last_pub_tenth = last;
            s_rooms[i].read_target = last;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "room table loaded (%s / %s / %s / %s)",
             s_rooms[0].cfg.name, s_rooms[1].cfg.name, s_rooms[2].cfg.name, s_rooms[3].cfg.name);
    return ESP_OK;
}

void heating_load_targets(void)
{
    /* Same table, same NVS: this exists so the first connect can re-read the
     * saved targets after the dashboard went up without them. */
    nvs_handle_t h;
    if (nvs_open(HEATING_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    for (int i = 0; i < HEATING_ROOMS; i++) {
        int16_t last = 0;
        if (nvs_get_i16(h, k_key_last[i], &last) == ESP_OK) {
            s_rooms[i].last_pub_tenth = last;
            if (!s_rooms[i].target.valid) s_rooms[i].read_target = last;
        }
    }
    nvs_close(h);
}

esp_err_t heating_save_cfg(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(HEATING_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (int i = 0; i < HEATING_ROOMS && err == ESP_OK; i++) {
        err = nvs_set_str(h, k_key_name[i], s_rooms[i].cfg.name);
        if (err == ESP_OK) err = nvs_set_str(h, k_key_trv[i], s_rooms[i].cfg.trv_topic);
        if (err == ESP_OK) err = nvs_set_str(h, k_key_sensor[i], s_rooms[i].cfg.sensor_topic);
        if (err == ESP_OK) err = nvs_set_i16(h, k_key_last[i], s_rooms[i].last_pub_tenth);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool heating_room_current(const heating_room_t *room, int16_t *tenth, bool *stale, int64_t *seen_ms)
{
    const heating_reading_t *use = NULL;
    /* The air sensor wins when it is the fresher of the two: it measures the
     * room, the TRV measures itself against the radiator. */
    if (room->room.valid) use = &room->room;
    if (room->valve.valid && (use == NULL || room->valve.seen_ms > use->seen_ms)) use = &room->valve;
    if (use == NULL) {
        if (stale) *stale = true;
        return false;
    }
    if (tenth) *tenth = use->tenth;
    if (seen_ms) *seen_ms = use->seen_ms;
    if (stale) *stale = (now_ms() - use->seen_ms) >= (int64_t)HEATING_STALE_S * 1000;
    return true;
}

bool heating_format_time(int64_t seen_ms, char *buf, size_t len)
{
    time_t t = time(NULL);
    if (t < 1600000000) {                       /* before SNTP: no wall clock */
        return false;
    }
    struct tm lt = {};
    localtime_r(&t, &lt);
    /* A reading older than the up-time we can measure is "earlier today". */
    int64_t up = (int64_t)lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    int64_t back = (now_ms() - seen_ms) / 1000;
    if (back > up) {
        snprintf(buf, len, "earlier");
        return true;
    }
    snprintf(buf, len, "%02d:%02d", lt.tm_hour, lt.tm_min);
    return true;
}

int16_t heating_room_target_ui(const heating_room_t *room)
{
    return room->read_target;
}

int heating_rooms_reporting(void)
{
    int n = 0;
    for (int i = 0; i < HEATING_ROOMS; i++) {
        int16_t tenth;
        bool stale;
        if (heating_room_current(&s_rooms[i], &tenth, &stale, NULL) && !stale) n++;
    }
    return n;
}

void heating_mark_all_stale(void)
{
    heating_ui_invalidate();
}

/* ── setpoints ───────────────────────────────────────────────────────── */

bool heating_online(void)
{
    return s_online && app_mqtt_configured();
}

static int16_t clamp_round(int16_t tenth)
{
    if (tenth < HEATING_MIN_TENTH) tenth = HEATING_MIN_TENTH;
    if (tenth > HEATING_MAX_TENTH) tenth = HEATING_MAX_TENTH;
    return (int16_t)(((tenth + HEATING_STEP_TENTH / 2) / HEATING_STEP_TENTH) * HEATING_STEP_TENTH);
}

/* zigbee2mqtt holds the target as degrees with one decimal, as a string. */
static void format_tenth(int16_t tenth, char *buf, size_t len)
{
    snprintf(buf, len, "%d.%d", tenth / 10, (tenth % 10 + 10) % 10);
}

int16_t heating_clamp_tenth(int16_t tenth)
{
    return clamp_round(tenth);
}

heating_req_result_t heating_request_target(int idx, int16_t tenth)
{
    heating_room_t *room = heating_room(idx);
    if (room == NULL) return HEATING_REQ_NO_ROOM;
    if (!heating_online()) return HEATING_REQ_OFFLINE;

    tenth = clamp_round(tenth);
    int64_t now = now_ms();
    if (tenth == room->last_pub_tenth &&
        now - room->last_pub_ms < (int64_t)HEATING_DUP_WINDOW_S * 1000) {
        return HEATING_REQ_ALREADY_SET;
    }

    char value[8];
    char payload[48];
    char cmd_topic[HEATING_TOPIC_MAX + 32];
    format_tenth(tenth, value, sizeof(value));
    /* The command topic, not the state topic: a setpoint written back into the
     * state JSON is only an echo, zigbee2mqtt never acts on it. */
    command_topic_for(room->cfg.trv_topic, cmd_topic, sizeof(cmd_topic));
    /* zigbee2mqtt's two command forms take different payloads: the
     * per-attribute topic (<device>/set/current_heating_setpoint) takes the
     * BARE value, the aggregate topic (<device>/set) takes the JSON object.
     * A JSON object on the per-attribute topic is rejected as "not a number". */
    int n;
    if (strstr(cmd_topic, "/set/") != NULL) {
        n = snprintf(payload, sizeof(payload), "%s", value);
    } else {
        n = snprintf(payload, sizeof(payload), "{\"current_heating_setpoint\": \"%s\"}", value);
    }
    if (app_mqtt_publish(cmd_topic, payload, n, 1, false) < 0) {
        return HEATING_REQ_OFFLINE;
    }
    room->last_pub_tenth = tenth;
    room->last_pub_ms = now;
    /* Nothing is claimed on screen: the card follows the TRV's own report. */
    ESP_LOGI(TAG, "%s -> %s  %s", room->cfg.name, value, cmd_topic);
    return HEATING_REQ_OK;
}

esp_err_t heating_permit_join(void)
{
    static const char payload[] = "{\"time\": 254}";
    int rc = app_mqtt_publish("zigbee2mqtt/bridge/request/permit_join", payload,
                              (int)strlen(payload), 1, false);
    return rc < 0 ? ESP_FAIL : ESP_OK;
}

/* ── inbound ─────────────────────────────────────────────────────────── */

static void apply_payload(heating_reading_t *reading, const char *data, int len, const char *key)
{
    char value[24];
    int16_t tenth = 0;
    if (!json_get(data, len, key, value, sizeof(value))) return;
    if (!parse_tenth(value, value + strlen(value), &tenth)) return;
    reading->tenth = tenth;
    reading->valid = true;
    reading->seen_ms = now_ms();
}

void heating_on_mqtt_message(const char *topic, int topic_len, const char *data, int data_len)
{
    /* Every message that arrives is logged with its topic and payload —
     * the first thing to look at when a card stays empty. */
    ESP_LOGI(TAG, "IN %.*s => %.*s",
             topic_len < 64 ? topic_len : 64, topic,
             data_len < 160 ? data_len : 160, data ? data : "");

    bool claimed = false;
    for (int pass = 0; pass < 2 && !claimed; pass++) {
        for (int i = 0; i < HEATING_ROOMS; i++) {
            heating_room_t *room = &s_rooms[i];
            const char *match = pass == 0 ? room->cfg.trv_topic : room->cfg.sensor_topic;
            if (match == NULL || match[0] == 0) continue;
            if ((int)strlen(match) != topic_len || memcmp(match, topic, topic_len) != 0) continue;

            claimed = true;
            if (pass == 0) {
                /* One zigbee2mqtt payload carries everything the card needs. */
                apply_payload(&room->target, data, data_len, "current_heating_setpoint");
                apply_payload(&room->valve, data, data_len, "local_temperature");
                char mode[16];
                room->mode = HEATING_HEATING;
                if (json_get(data, data_len, "system_mode", mode, sizeof(mode))) {
                    if (strcasecmp(mode, "off") == 0) room->mode = HEATING_MODE_OFF;
                }
                if (room->target.valid) room->read_target = room->target.tenth;
            } else {
                apply_payload(&room->room, data, data_len, "temperature");
            }
            /* Whatever the product mirrors changed: wake the screen and repaint. */
            heating_ui_poke();
            return;                       /* one room owns a topic */
        }
    }
    ESP_LOGD(TAG, "message on a topic no room watches — ignored");
}

void heating_broker_ready(bool online)
{
    bool was_online = s_online;
    s_online = online;
    app_mqtt_set_online(online);
    heating_ui_sync_online();
    heating_ui_invalidate();
    if (!online) {
        /* Values freeze where they are; the footer says the link is down. */
        if (was_online) ESP_LOGW(TAG, "broker session lost");
        return;
    }

    /* A fresh session: the cards start from the saved targets until the TRVs
     * report, then every room's one zigbee2mqtt state topic is subscribed. */
    heating_load_targets();
    int subscribed = 0;
    for (int i = 0; i < HEATING_ROOMS; i++) {
        if (s_rooms[i].cfg.trv_topic[0]) subscribed += (app_mqtt_subscribe(s_rooms[i].cfg.trv_topic) >= 0);
        if (s_rooms[i].cfg.sensor_topic[0]) subscribed += (app_mqtt_subscribe(s_rooms[i].cfg.sensor_topic) >= 0);
        /* zigbee2mqtt answers 'bridge/devices' with the bridge's device table;
         * not needed per room, but it is the first thing an operator wants to
         * see when no state ever arrives. */
        ESP_LOGD(TAG, "%s: state '%s', setpoint command '%s'", s_rooms[i].cfg.name,
                 s_rooms[i].cfg.trv_topic, heating_room_command_topic(i));
    }
    app_mqtt_subscribe("zigbee2mqtt/bridge/devices");
    /* These TRVs are Tuya: their attributes carry no GET access, so the /get
     * poll below goes unanswered, and zigbee2mqtt does not retain device state
     * unless asked — a freshly booted panel would stay blank until a valve
     * happens to report. Ask the bridge to RETAIN each room device's state
     * publishes (persisted in its configuration, idempotent to repeat): from
     * the device's next report on, every boot syncs instantly from the broker. */
    for (int i = 0; i < HEATING_ROOMS; i++) {
        static const char kPrefix[] = "zigbee2mqtt/";
        const char *t = s_rooms[i].cfg.trv_topic;
        if (strncmp(t, kPrefix, sizeof(kPrefix) - 1) != 0) continue;
        char req[HEATING_TOPIC_MAX + 48];
        int rn = snprintf(req, sizeof(req),
                          "{\"id\": \"%s\", \"options\": {\"retain\": true}}",
                          t + sizeof(kPrefix) - 1);
        app_mqtt_publish("zigbee2mqtt/bridge/request/device/options", req, rn, 0, false);
    }
    /* zigbee2mqtt only publishes a state topic when something changes, so
     * nobody would hear from a TRV until a valve moves — request the current
     * state of every topic, as a Home Assistant device does when it starts. */
    int polled = heating_request_states();
    ESP_LOGI(TAG, "subscribed to %d room topic(s); requested fresh state of %d", subscribed, polled);
    heating_log_diag();
}

/* Ask zigbee2mqtt for a device's current state: a read request on the
 * device's /get topic, naming the attributes the cards parse. A message on
 * the bare state topic does nothing — zigbee2mqtt only ever PUBLISHES there,
 * and this panel is subscribed to it, so the old empty-payload "poll" went
 * nowhere except straight back into our own handler. */
static bool request_state_of(const char *state_topic, const char *attrs_json)
{
    char get_topic[HEATING_TOPIC_MAX + 8];
    snprintf(get_topic, sizeof(get_topic), "%s/get", state_topic);
    return app_mqtt_publish(get_topic, attrs_json, (int)strlen(attrs_json), 0, false) >= 0;
}

int heating_request_states(void)
{
    int n = 0;
    for (int i = 0; i < HEATING_ROOMS; i++) {
        heating_room_t *room = &s_rooms[i];
        if (room->cfg.trv_topic[0] &&
            request_state_of(room->cfg.trv_topic,
                             "{\"current_heating_setpoint\": \"\", \"local_temperature\": \"\"}")) n++;
        if (room->cfg.sensor_topic[0] &&
            request_state_of(room->cfg.sensor_topic, "{\"temperature\": \"\"}")) n++;
    }
    if (n > 0) ESP_LOGI(TAG, "state requested for %d topic(s)", n);
    return n;
}

/* ── persistence: factory reset ──────────────────────────────────────── */

void heating_factory_reset(void)
{
    /* Credentials, broker and the room table are all of the device's own
     * memory; the TRVs keep their setpoints. */
    ESP_LOGW(TAG, "factory reset: wiping settings");
    nvs_flash_erase();
    esp_restart();
}

void heating_log_diag(void)
{
    ESP_LOGI(TAG, "--- mqtt diag: wifi '%s', broker '%s', user '%s', session %s, %d/%d rooms reporting",
             app_mqtt_wifi_missing() ? "(none)" : "configured",
             app_mqtt_broker_uri(),
             app_mqtt_user()[0] ? app_mqtt_user() : "(none)",
             app_mqtt_session_up() ? "up" : "down",
             heating_rooms_reporting(), HEATING_ROOMS);
    for (int i = 0; i < HEATING_ROOMS; i++) {
        int16_t tenth = 0;
        bool stale = true;
        int64_t seen = 0;
        bool have = heating_room_current(&s_rooms[i], &tenth, &stale, &seen);
        ESP_LOGI(TAG, "  %-12s state '%s'  setpoint '%s'  sensor '%s'",
                 s_rooms[i].cfg.name, s_rooms[i].cfg.trv_topic,
                 heating_room_command_topic(i),
                 s_rooms[i].cfg.sensor_topic[0] ? s_rooms[i].cfg.sensor_topic : "(none)");
        ESP_LOGI(TAG, "  %-12s target %.1f, setpoint reported %s, current %s%.1f%s (mode %s)",
                 s_rooms[i].cfg.name, s_rooms[i].read_target / 10.0,
                 s_rooms[i].target.valid ? "yes" : "no",
                 have ? "" : "none ", have ? tenth / 10.0 : 0.0,
                 (have && stale) ? " (stale)" : "",
                 s_rooms[i].mode == HEATING_HEATING ? "heat" : "off");
    }
}

void heating_reload_cfg(void)
{
    /* Only the card targets need re-reading: the table itself is in RAM and
     * was just written by the settings page. */
    heating_load_targets();
    heating_ui_invalidate();
}
