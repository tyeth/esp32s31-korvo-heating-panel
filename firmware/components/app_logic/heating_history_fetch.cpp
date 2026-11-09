/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The detail chart's backfill: the panel samples temperatures only while it is
 * running, but Home Assistant's recorder has been watching the same MQTT
 * topics for months. Once per boot (when the broker session first comes up)
 * this fetches each room's last 48 h from HA's REST history API and seeds the
 * ring, so the chart is full from the first tap instead of growing from zero.
 *
 * Needs a long-lived access token (HA profile -> Security), stored with the
 * base URL in NVS: console `ha-config <url> <token>` / `ha-backfill`, or the
 * settings page. The response is scanned with strstr, never parsed into a
 * JSON tree: 48 h of climate states runs to ~1 MB, and a cJSON tree of that
 * is tens of thousands of small nodes, which the SPIRAM-first malloc policy
 * routes into the internal heap — the one allocation pattern this firmware
 * cannot afford. The fetch task's own stack is PSRAM, so NOTHING here may
 * write NVS (a flash write with a PSRAM stack resets the chip); the config
 * writes happen on the caller's task.
 */

#include "heating.h"
#include "app_mqtt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <esp_log.h>
#include <esp_console.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

static const char *TAG = "heating_hist";

#define HA_NVS_NS       "heating"
#define HA_URL_MAX      96
#define HA_TOKEN_MAX    224
#define HA_RESP_MAX     (4 * 1024 * 1024)
#define HA_WINDOW_S     ((int64_t)HEATING_HIST_LEN * HEATING_HIST_PERIOD_S)

static char s_ha_url[HA_URL_MAX]     = "https://homeassistant.gdenu.fi:8123";
static char s_ha_token[HA_TOKEN_MAX] = "";
static bool s_loaded = false;
static volatile bool s_running = false;

static void ha_load(void)
{
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(HA_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_ha_url);
    nvs_get_str(h, "haurl", s_ha_url, &n);
    n = sizeof(s_ha_token);
    nvs_get_str(h, "hatok", s_ha_token, &n);
    nvs_close(h);
}

const char *heating_ha_url(void)
{
    ha_load();
    return s_ha_url;
}

bool heating_ha_has_token(void)
{
    ha_load();
    return s_ha_token[0] != 0;
}

esp_err_t heating_ha_config_set(const char *url, const char *token)
{
    ha_load();
    nvs_handle_t h;
    if (nvs_open(HA_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    if (url && url[0]) {
        if (strlen(url) >= sizeof(s_ha_url)) { nvs_close(h); return ESP_ERR_INVALID_SIZE; }
        snprintf(s_ha_url, sizeof(s_ha_url), "%s", url);
        nvs_set_str(h, "haurl", s_ha_url);
    }
    if (token && token[0]) {
        if (strlen(token) >= sizeof(s_ha_token)) { nvs_close(h); return ESP_ERR_INVALID_SIZE; }
        snprintf(s_ha_token, sizeof(s_ha_token), "%s", token);
        nvs_set_str(h, "hatok", s_ha_token);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "HA recorder config saved (url %s, token %s)",
             s_ha_url, s_ha_token[0] ? "set" : "missing");
    return ESP_OK;
}

/* "2026-11-09T18:21:33.123456+00:00" (UTC) -> epoch seconds, -1 on nonsense.
 * The device runs on UTC (no TZ is ever set), so mktime is timegm here. */
static int64_t iso_to_epoch(const char *s)
{
    struct tm tm = {};
    int y, mo, d, h, mi, se;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return -1;
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = se;
    time_t t = mktime(&tm);
    return t > 0 ? (int64_t)t : -1;
}

/** GET one room's history and seed the ring. Returns seeded count, -1 on a
 *  transport/HTTP error. */
static int fetch_room(int room, const char *entity, int64_t start_epoch)
{
    char url[256];
    struct tm tm;
    time_t start = (time_t)start_epoch;
    gmtime_r(&start, &tm);
    snprintf(url, sizeof(url),
             "%s/api/history/period/%04d-%02d-%02dT%02d:%02d:%02dZ?filter_entity_id=%s",
             s_ha_url, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, entity);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = 30000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) return -1;

    char auth[HA_TOKEN_MAX + 8];
    snprintf(auth, sizeof(auth), "Bearer %s", s_ha_token);
    esp_http_client_set_header(client, "Authorization", auth);

    int seeded = -1;
    char *body = NULL;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(client);

        size_t cap = 256 * 1024, w = 0;
        body = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (body == NULL) break;
        while (true) {
            if (w + 4096 + 1 > cap) {
                if (cap >= HA_RESP_MAX) { ESP_LOGW(TAG, "%s: response over %d — truncated", entity, HA_RESP_MAX); break; }
                cap *= 2;
                char *nb = (char *)heap_caps_realloc(body, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (nb == NULL) break;
                body = nb;
            }
            int r = esp_http_client_read(client, body + w, 4096);
            if (r <= 0) break;
            w += (size_t)r;
        }
        body[w] = '\0';
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d (%.120s)", entity, status, body);
            break;
        }

        /* Scan, don't parse: pairs of "current_temperature": <number> followed
         * by that state's "last_updated": "<iso>". */
        seeded = 0;
        const char *p = body;
        while ((p = strstr(p, "\"current_temperature\"")) != NULL) {
            p += 21;
            while (*p == ':' || *p == ' ') p++;
            char *endp = NULL;
            double c = strtod(p, &endp);
            bool have = (endp != p);              /* null / junk -> skip */
            const char *q = strstr(p, "\"last_updated\"");
            if (q == NULL) break;
            q += 14;
            while (*q == ':' || *q == ' ' || *q == '"') q++;
            int64_t epoch = iso_to_epoch(q);
            if (have && epoch > 0) {
                int rc = heating_history_seed(room, epoch,
                                              (int16_t)(c * 10.0 + (c >= 0 ? 0.5 : -0.5)));
                if (rc > 0) seeded++;
            }
            p = q;
        }
    } while (false);

    free(body);
    esp_http_client_cleanup(client);
    return seeded;
}

static void backfill_task(void *arg)
{
    (void)arg;
    /* The seed slots are computed from wall-clock time. */
    for (int i = 0; i < 120 && time(NULL) < 1600000000; i++) vTaskDelay(pdMS_TO_TICKS(500));
    if (time(NULL) < 1600000000) {
        ESP_LOGW(TAG, "no SNTP time — backfill skipped");
        s_running = false;
        vTaskDelete(NULL);
        return;
    }

    int64_t start = (int64_t)time(NULL) - HA_WINDOW_S;
    for (int i = 0; i < HEATING_ROOMS; i++) {
        heating_room_t *room = heating_room(i);
        const char *topic = room->cfg.trv_topic;
        if (strncmp(topic, "zigbee2mqtt/", 12) != 0) continue;
        char entity[80];
        snprintf(entity, sizeof(entity), "climate.%s", topic + 12);
        int n = fetch_room(i, entity, start);
        ESP_LOGI(TAG, "%s (%s): %s%d samples backfilled", room->cfg.name, entity,
                 n < 0 ? "FAILED, " : "", n < 0 ? 0 : n);
    }
    heating_ui_invalidate();
    s_running = false;
    vTaskDelete(NULL);
}

void heating_history_backfill_start(void)
{
    ha_load();
    if (s_ha_token[0] == 0) {
        ESP_LOGI(TAG, "no HA token stored — chart history starts at boot "
                      "(ha-config <url> <token> to enable backfill)");
        return;
    }
    if (s_running) return;
    s_running = true;
    /* PSRAM stack, like the agent's: TLS + a megabyte-scale scan, no NVS. */
    TaskHandle_t h = NULL;
    if (xTaskCreateWithCaps(backfill_task, "ha_backfill", 16384, NULL, 2, &h,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_running = false;
        ESP_LOGW(TAG, "backfill task not started");
    }
}

/* ── console ─────────────────────────────────────────────────────────── */

static int cmd_ha_config(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ha-config <base url> [token]\n       current: %s, token %s\n",
               heating_ha_url(), heating_ha_has_token() ? "set" : "missing");
        return 1;
    }
    esp_err_t err = heating_ha_config_set(argv[1], argc > 2 ? argv[2] : NULL);
    printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_ha_backfill(int argc, char **argv)
{
    (void)argc; (void)argv;
    heating_history_backfill_start();
    printf("backfill %s — watch the log\n", heating_ha_has_token() ? "started" : "needs ha-config first");
    return 0;
}

void heating_history_fetch_register_console(void)
{
    esp_console_cmd_t c = {};
    c.command = "ha-config";
    c.help = "Home Assistant URL + long-lived token for chart backfill";
    c.func = cmd_ha_config;
    esp_console_cmd_register(&c);

    esp_console_cmd_t b = {};
    b.command = "ha-backfill";
    b.help = "fetch 48h of temperatures from Home Assistant into the charts";
    b.func = cmd_ha_backfill;
    esp_console_cmd_register(&b);
}
