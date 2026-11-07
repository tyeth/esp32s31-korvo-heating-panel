/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The settings page on port 80.
 *
 * This is where the panel is set up — Wi-Fi, the broker and every room's
 * topics live here, not on the panel: the touch screen only ever shows the
 * four cards.
 *
 *   GET  /settings                       the form, pre-filled from NVS
 *   POST /settings                       apply it (form-encoded), save
 *   POST /settings?action=permit_join    ask zigbee2mqtt to permit a device
 *
 * The page re-points the panel's Wi-Fi and broker, so changes apply only to
 * the form this page posts (its `submit` button) or its permit-join button;
 * there is no other path into it.
 *
 * On a panel with no Wi-Fi stored the page is served over the MQTT block's
 * onboarding access point (app_mqtt_ap_*); the access point's name is printed
 * in the boot log; a BOOT long press brings it back after its idle timeout.
 */

#include "heating.h"
#include "app_mqtt.h"
#include "app_webui.h"
#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "heating_web";

/* The form: one entry per field, the key exactly as the page names it. */
#define FORM_FIELDS         (5 + HEATING_ROOMS * 3)
#define FORM_VALUE_MAX      (HEATING_TOPIC_MAX + 8)

typedef struct {
    const char *key;
    char        value[FORM_VALUE_MAX];
} form_field_t;

static form_field_t s_form[FORM_FIELDS];
static bool s_submit = false;
static bool s_permit = false;

static const char *const k_field_keys[FORM_FIELDS] = {
    "ssid", "pass", "broker", "apass", "ca",
    "r0name", "r0trv", "r0sensor",
    "r1name", "r1trv", "r1sensor",
    "r2name", "r2trv", "r2sensor",
    "r3name", "r3trv", "r3sensor",
};

/* ── form decoding ───────────────────────────────────────────────────── */

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, size_t len, char *dst, size_t dst_len)
{
    size_t w = 0;
    for (size_t i = 0; i < len && w + 1 < dst_len; i++) {
        if (src[i] == '+') {
            dst[w++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            int hi = hex_val(src[i + 1]), lo = hex_val(src[i + 2]);
            if (hi < 0 || lo < 0) { dst[w++] = src[i]; continue; }
            dst[w++] = (char)((hi << 4) | lo);
            i += 2;
        } else {
            dst[w++] = src[i];
        }
    }
    dst[w] = '\0';
}

static void form_reset(void)
{
    for (int i = 0; i < FORM_FIELDS; i++) {
        s_form[i].key = k_field_keys[i];
        s_form[i].value[0] = '\0';
    }
    s_submit = false;
    s_permit = false;
}

static const char *form_get(const char *key)
{
    for (int i = 0; i < FORM_FIELDS; i++) {
        if (strcmp(s_form[i].key, key) == 0) return s_form[i].value;
    }
    return "";
}

static void form_set(const char *key, size_t klen, const char *val, size_t vlen)
{
    if (klen == 6 && strncmp(key, "submit", 6) == 0) { s_submit = true; return; }
    if (klen == 6 && strncmp(key, "permit", 6) == 0) { s_permit = true; return; }
    for (int i = 0; i < FORM_FIELDS; i++) {
        if (strlen(s_form[i].key) != klen || strncmp(s_form[i].key, key, klen) != 0) continue;
        url_decode(val, vlen, s_form[i].value, sizeof(s_form[i].value));
        return;
    }
}

static void form_parse(const char *body)
{
    const char *p = body;
    while (*p) {
        const char *eq = strchr(p, '=');
        if (eq == NULL) break;
        const char *amp = strchr(eq, '&');
        size_t klen = (size_t)(eq - p);
        size_t vlen = amp ? (size_t)(amp - eq - 1) : strlen(eq + 1);
        form_set(p, klen, eq + 1, vlen);
        if (amp == NULL) break;
        p = amp + 1;
    }
}

/* ── rendering ───────────────────────────────────────────────────────── */

static void html_escape(const char *in, char *out, size_t len)
{
    size_t w = 0;
    for (const char *p = in; *p && w + 8 < len; p++) {
        switch (*p) {
            case '&':  w += (size_t)snprintf(out + w, len - w, "&amp;");  break;
            case '<':  w += (size_t)snprintf(out + w, len - w, "&lt;");   break;
            case '>':  w += (size_t)snprintf(out + w, len - w, "&gt;");   break;
            case '"':  w += (size_t)snprintf(out + w, len - w, "&quot;"); break;
            default:   out[w++] = *p; break;
        }
    }
    out[w] = '\0';
}

static void field(const char *key, const char *label, const char *value, const char *placeholder)
{
    char safe[(HEATING_TOPIC_MAX * 6) + 1];
    char line[(HEATING_TOPIC_MAX * 6) + 256];
    html_escape(value, safe, sizeof(safe));
    if (placeholder != NULL) {
        snprintf(line, sizeof(line),
                 "<label>%s<br><input name=\"%s\" value=\"%s\" placeholder=\"%s\"></label>",
                 label, key, safe, placeholder);
    } else {
        snprintf(line, sizeof(line), "<label>%s<br><input name=\"%s\" value=\"%s\"></label>",
                 label, key, safe);
    }
    app_webui_send_chunk(line);
}

static void render_page(bool saved)
{
    char line[(HEATING_TOPIC_MAX * 6) + 256];
    app_webui_chunk_begin("text/html; charset=utf-8");
    app_webui_send_chunk(
        "<!DOCTYPE html><html><head><meta name=viewport "
        "content='width=device-width,initial-scale=1'><title>Heating panel settings</title>"
        "<style>body{font-family:system-ui;margin:0;background:#141414;color:#eee}"
        "h1{font-size:18px;padding:14px 16px;margin:0;border-bottom:1px solid #333;color:#e0a06a}"
        "h2{font-size:15px;margin:22px 16px 6px;color:#9ad0f5}"
        "form{padding:0 16px 24px}label{display:block;margin:12px 0 0;font-size:13px;color:#bbb}"
        "input{width:100%;padding:9px;margin-top:4px;background:#1e1e1e;color:#eee;"
        "border:1px solid #333;border-radius:6px;box-sizing:border-box}"
        "button{margin-top:18px;padding:11px 18px;background:#e0a06a;color:#111;border:0;"
        "border-radius:6px;font-size:15px}button.alt{background:#333;color:#eee;margin-left:8px}"
        "</style></head><body><h1>Heating panel settings</h1>");

    if (saved) {
        app_webui_send_chunk("<p style='padding:14px 16px;color:#7fd4a3'>Saved. The panel "
                             "reconnects with these settings — the dashboard fills in within "
                             "a few seconds.</p>");
    }
    if (!app_mqtt_configured()) {
        char ap[33];
        app_mqtt_ap_ssid(ap, sizeof(ap));
        if (ap[0]) {
            snprintf(line, sizeof(line),
                     "<p style='padding:0 16px;color:#e0a06a'>Not online yet. On a fresh panel "
                     "join the access point <b>%s</b> (the password is in the boot log) and "
                     "load http://korvo-heating.local/settings, then set Wi-Fi and the broker "
                     "here.</p>", ap);
            app_webui_send_chunk(line);
        } else {
            app_webui_send_chunk("<p style='padding:0 16px;color:#e0a06a'>Set Wi-Fi and the "
                                 "broker here: until both are filled in the panel stays "
                                 "offline.</p>");
        }
    }

    app_webui_send_chunk("<form method=post action='/settings'>");

    app_webui_send_chunk("<h2>Wi-Fi</h2>");
    app_webui_send_chunk("<label>Network<br><input name=\"ssid\" placeholder=\"home Wi-Fi\"></label>");
    app_webui_send_chunk("<label>Password<br><input name=\"pass\" type=password "
                         "placeholder=\"leave empty to keep the current one\"></label>");

    app_webui_send_chunk("<h2>MQTT broker</h2>");
    field("broker", "Broker URI (mqtts:// for TLS)", app_mqtt_broker_uri(),
          "mqtts://homeassistant.gdenu.fi:8123");
    app_webui_send_chunk("<label>Broker password<br><input name=\"apass\" type=password "
                         "placeholder=\"leave empty to keep the current one\"></label>");
    app_webui_send_chunk("<p style='font-size:12px;color:#888'>TLS is used for an mqtts:// URI. "
                         "Because the server's certificate is issued for a different name than "
                         "the one dialled, the name is not verified; the chain still is.</p>");
    app_webui_send_chunk("<label>Broker root CA, PEM (optional — the device's built-in pack is "
                         "used when empty; '-' clears a stored one)<br><textarea name=\"ca\" rows=4 "
                         "style=\"width:100%;background:#1e1e1e;color:#eee;border:1px solid "
                         "#333;border-radius:6px\"></textarea></label>");

    static const char *const title[HEATING_ROOMS] = {
        "<h2>Bathroom</h2>", "<h2>Hallway</h2>", "<h2>Office</h2>", "<h2>Living room</h2>"
    };
    for (int i = 0; i < HEATING_ROOMS; i++) {
        heating_room_t *r = heating_room(i);
        app_webui_send_chunk(title[i]);
        char key[16];
        snprintf(key, sizeof(key), "r%dname", i);
        field(key, "Display name", r->cfg.name, NULL);
        snprintf(key, sizeof(key), "r%dtrv", i);
        field(key, "TRV state topic (setpoints are written here)", r->cfg.trv_topic,
              "zigbee2mqtt/0x…");
        snprintf(key, sizeof(key), "r%dsensor", i);
        field(key, "Room air-sensor state topic", r->cfg.sensor_topic, "zigbee2mqtt/0x…");
    }

    app_webui_send_chunk("<button type=submit name=submit value=1>Save</button>");
    app_webui_send_chunk("<button class=alt type=submit name=permit value=1 "
                         "formaction='/settings?action=permit_join'>Permit join (4 min)</button>");
    app_webui_send_chunk("</form></body></html>");
    app_webui_chunk_end();
}

static void render_message(const char *text)
{
    char line[256];
    app_webui_chunk_begin("text/html; charset=utf-8");
    app_webui_send_chunk("<!DOCTYPE html><html><body style='font-family:system-ui;"
                         "background:#141414;color:#eee;padding:24px'>");
    snprintf(line, sizeof(line), "<h1 style='color:#e0a06a'>%s</h1>"
                                 "<p><a style='color:#9ad0f5' href='/settings'>Back</a></p>",
             text);
    app_webui_send_chunk(line);
    app_webui_send_chunk("</body></html>");
    app_webui_chunk_end();
}

/* ── applying the form ───────────────────────────────────────────────── */

static void apply_form(void)
{
    /* Every field is validated against its destination before anything is
     * written: an over-long topic on the page is refused as a whole, so the
     * stored table never holds a silently truncated one. */
    for (int i = 0; i < HEATING_ROOMS; i++) {
        heating_room_t *r = heating_room(i);
        char key[16];

        snprintf(key, sizeof(key), "r%dname", i);
        if (strlen(form_get(key)) >= sizeof(r->cfg.name)) {
            ESP_LOGW(TAG, "%s: name too long (%d chars) — form refused", key, (int)strlen(form_get(key)));
            return;
        }
        snprintf(key, sizeof(key), "r%dtrv", i);
        if (strlen(form_get(key)) >= sizeof(r->cfg.trv_topic)) {
            ESP_LOGW(TAG, "%s: topic too long (%d chars, max %d) — form refused",
                     key, (int)strlen(form_get(key)), (int)sizeof(r->cfg.trv_topic) - 1);
            return;
        }
        snprintf(key, sizeof(key), "r%dsensor", i);
        if (strlen(form_get(key)) >= sizeof(r->cfg.sensor_topic)) {
            ESP_LOGW(TAG, "%s: topic too long (%d chars, max %d) — form refused",
                     key, (int)strlen(form_get(key)), (int)sizeof(r->cfg.sensor_topic) - 1);
            return;
        }
    }

    /* Topics first: they are what makes a card work, and a broker edit below
     * restarts the client. */
    for (int i = 0; i < HEATING_ROOMS; i++) {
        heating_room_t *r = heating_room(i);
        char key[16];

        snprintf(key, sizeof(key), "r%dname", i);
        if (form_get(key)[0]) {
            strncpy(r->cfg.name, form_get(key), sizeof(r->cfg.name) - 1);
            r->cfg.name[sizeof(r->cfg.name) - 1] = '\0';
        }
        snprintf(key, sizeof(key), "r%dtrv", i);
        if (form_get(key)[0]) {
            strncpy(r->cfg.trv_topic, form_get(key), sizeof(r->cfg.trv_topic) - 1);
            r->cfg.trv_topic[sizeof(r->cfg.trv_topic) - 1] = '\0';
        }
        snprintf(key, sizeof(key), "r%dsensor", i);
        if (form_get(key)[0]) {
            strncpy(r->cfg.sensor_topic, form_get(key), sizeof(r->cfg.sensor_topic) - 1);
            r->cfg.sensor_topic[sizeof(r->cfg.sensor_topic) - 1] = '\0';
        }
    }
    /* Saved BEFORE the transport below is touched, so a session that comes up
     * during/right after the restart already sees the new table. */
    heating_save_cfg();

    if (form_get("apass")[0] || form_get("ca")[0]) {
        /* A new broker password or CA: the session is rebuilt around it. */
        const char *pass = form_get("apass")[0] ? form_get("apass") : app_mqtt_user_password();
        esp_err_t err = app_mqtt_set_broker(form_get("broker")[0] ? form_get("broker")
                                                                 : app_mqtt_broker_uri(),
                                           app_mqtt_user(), pass);
        if (err != ESP_OK) ESP_LOGW(TAG, "broker not saved: %s", esp_err_to_name(err));
        /* "-" clears a pinned CA (the PEM itself may contain '-' characters, so
         * only a lone one is the sentinel — the same rule the broker password
         * uses in mqtt-setup). */
        if (strcmp(form_get("ca"), "-") == 0) {
            app_mqtt_set_broker_ca(NULL);
        } else if (form_get("ca")[0]) {
            app_mqtt_set_broker_ca(form_get("ca"));
        }
    } else if (form_get("broker")[0]) {
        esp_err_t err = app_mqtt_set_broker(form_get("broker"), app_mqtt_user(),
                                           app_mqtt_user_password());
        if (err != ESP_OK) ESP_LOGW(TAG, "broker not saved: %s", esp_err_to_name(err));
    }

    if (form_get("ssid")[0]) {
        /* An empty password keeps the stored one: the page never shows it. */
        esp_err_t err = app_mqtt_set_wifi(form_get("ssid"), form_get("pass"));
        if (err != ESP_OK) ESP_LOGW(TAG, "wifi not saved: %s", esp_err_to_name(err));
    }
}

/* ── the request ─────────────────────────────────────────────────────── */

static esp_err_t settings_entry(void)
{
    form_reset();

    char body[3000];
    int total = 0, n;
    while (total < (int)sizeof(body) - 1 &&
           (n = app_webui_read_request(body + total, sizeof(body) - 1 - total)) > 0) {
        total += n;
    }
    body[total] = '\0';

    /* A body that did not fit would be applied a field short, and a half
     * decoded topic is worse than a rejected form: change nothing. */
    if (total >= (int)sizeof(body) - 1) {
        render_message("Form too long \xE2\x80\x94 nothing was changed");
        return ESP_OK;
    }
    if (total > 0) form_parse(body);

    if (s_permit) {
        esp_err_t err = heating_permit_join();
        render_message(err == ESP_OK ? "Permit join sent — 4 minutes"
                                     : "Offline — nothing was sent");
        return ESP_OK;
    }
    if (s_submit) {
        /* Applied here and now: the settings page is reached over the link the
         * form configures, so nothing has to be restarted to keep it. */
        apply_form();
        heating_reload_cfg();
        render_page(true);
        return ESP_OK;
    }
    render_page(false);
    return ESP_OK;
}

void heating_web_install(void)
{
    app_webui_set_settings_handler(settings_entry);
    ESP_LOGI(TAG, "settings page at http://<device>/settings");
}
