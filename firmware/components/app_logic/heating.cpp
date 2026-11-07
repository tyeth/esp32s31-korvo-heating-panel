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
    format_tenth(tenth, value, sizeof(value));
    int n = snprintf(payload, sizeof(payload), "{\"current_heating_setpoint\": \"%s\"}", value);
    if (app_mqtt_publish(room->cfg.trv_topic, payload, n, 1, false) < 0) {
        return HEATING_REQ_OFFLINE;
    }
    room->last_pub_tenth = tenth;
    room->last_pub_ms = now;
    /* Nothing is claimed on screen: the card follows the TRV's own report. */
    ESP_LOGI(TAG, "%s -> %s  %s", room->cfg.name, value, room->cfg.trv_topic);
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
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < HEATING_ROOMS; i++) {
            heating_room_t *room = &s_rooms[i];
            const char *match = pass == 0 ? room->cfg.trv_topic : room->cfg.sensor_topic;
            if (match == NULL || match[0] == 0) continue;
            if ((int)strlen(match) != topic_len || memcmp(match, topic, topic_len) != 0) continue;

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
            return;
        }
    }
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
    for (int i = 0; i < HEATING_ROOMS; i++) {
        if (s_rooms[i].cfg.trv_topic[0]) app_mqtt_subscribe(s_rooms[i].cfg.trv_topic);
        if (s_rooms[i].cfg.sensor_topic[0]) app_mqtt_subscribe(s_rooms[i].cfg.sensor_topic);
    }
    ESP_LOGI(TAG, "subscribed to %d room topics", HEATING_ROOMS);
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

void heating_reload_cfg(void)
{
    /* Only the card targets need re-reading: the table itself is in RAM and
     * was just written by the settings page. */
    heating_load_targets();
    heating_ui_invalidate();
}
