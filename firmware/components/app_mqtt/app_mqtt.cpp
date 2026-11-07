#include "app_mqtt.h"
#include "app_driver.h"
#include "app_config.h"
#include "app_console.h"

#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_console.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <nvs.h>
#include <mqtt_client.h>


static const char *TAG = "app_mqtt";

static app_driver_handle_t s_handle = 0;
static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
static bool s_wifi_started = false;

/* Node identity: station MAC as lowercase hex — stable, unique per board. */
static char s_node_id[13] = {0};
static char s_status_topic[48] = {0};

/* NVS-backed config (namespace "zc_mqtt"), set via the console commands. */
static char s_ssid[33] = {0};
static char s_pass[65] = {0};
static char s_uri[129] = {0};
static char s_user[33] = {0};
static char s_mqtt_pass[65] = {0};



/* Connection-state transitions. Behavior blocks hook these via
 * mqtt_event_cases — `connected` is in scope (1 = broker session up). */
static void mqtt_on_event(int connected)
{
    (void)connected;

}

/* Hardware/param bus -> state topics. Publishes only while connected;
 * mqtt_device_create republishes current state on every (re)connect, so
 * nothing is lost while offline. */
static void mqtt_driver_cb(
    app_driver_param_id_t param_id,
    app_driver_param_val_t val,
    app_driver_handle_t source,
    void *ctx)
{
    (void)source;
    (void)ctx;
    (void)val;
    if (!s_connected || s_client == NULL) return;
    switch (param_id) {

    default:
        break;
    }
}



/* Command topics -> hardware. Runs on the MQTT task; payloads are not
 * NUL-terminated, so every case gets (topic, tlen, data, dlen). */
static void mqtt_handle_data(const char *topic, int tlen, const char *data, int dlen)
{
    (void)topic; (void)tlen; (void)data; (void)dlen;

}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "MQTT connected");
            esp_mqtt_client_publish(s_client, s_status_topic, "online", 0, 1, true);

            mqtt_on_event(1);
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "MQTT disconnected — esp-mqtt reconnects on its own");
            mqtt_on_event(0);
            break;
        case MQTT_EVENT_DATA:
            mqtt_handle_data(event->topic, event->topic_len, event->data, event->data_len);
            break;
        default:
            break;
    }
}

static void mqtt_start_client(void)
{
    if (s_client != NULL || s_uri[0] == 0) return;
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = s_uri;
    if (s_user[0]) cfg.credentials.username = s_user;
    if (s_mqtt_pass[0]) cfg.credentials.authentication.password = s_mqtt_pass;
    cfg.session.last_will.topic = s_status_topic;
    cfg.session.last_will.msg = "offline";
    cfg.session.last_will.qos = 1;
    cfg.session.last_will.retain = true;
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt client init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT connecting to %s", s_uri);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected — retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi got IP");
        mqtt_start_client();
    }
}

static void mqtt_wifi_start(void)
{
    if (s_wifi_started || s_ssid[0] == 0) return;
    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
}

/* ── console config: the onboarding surface ───────────────────────── */

static void mqtt_load_config(void)
{
    nvs_handle_t h;
    if (nvs_open("zc_mqtt", NVS_READONLY, &h) != ESP_OK) return;
    size_t n;
    n = sizeof(s_ssid);      nvs_get_str(h, "ssid", s_ssid, &n);
    n = sizeof(s_pass);      nvs_get_str(h, "pass", s_pass, &n);
    n = sizeof(s_uri);       nvs_get_str(h, "uri", s_uri, &n);
    n = sizeof(s_user);      nvs_get_str(h, "user", s_user, &n);
    n = sizeof(s_mqtt_pass); nvs_get_str(h, "mpass", s_mqtt_pass, &n);
    nvs_close(h);
}

/* Public setters (app_mqtt.h): validate, persist, apply. The console
 * commands below are thin wrappers, and mqtt-setup asks for the same values
 * one at a time — so a product never needs to know the NVS layout, and "the
 * device asks for its settings" is a command it already has. */
esp_err_t app_mqtt_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == 0) return ESP_ERR_INVALID_ARG;
    if (password == NULL) password = "";
    if (strlen(ssid) >= sizeof(s_ssid) || strlen(password) >= sizeof(s_pass)) return ESP_ERR_INVALID_SIZE;
    nvs_handle_t h;
    esp_err_t err = nvs_open("zc_mqtt", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password, sizeof(s_pass));
    if (s_wifi_started) {
        wifi_config_t wc = {};
        strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        esp_wifi_disconnect();
        esp_wifi_connect();
    } else {
        mqtt_wifi_start();
    }
    return ESP_OK;
}

esp_err_t app_mqtt_set_broker(const char *uri, const char *username, const char *password)
{
    if (uri == NULL || uri[0] == 0) return ESP_ERR_INVALID_ARG;
    if (username == NULL) username = "";
    if (password == NULL) password = "";
    if (strlen(uri) >= sizeof(s_uri) || strlen(username) >= sizeof(s_user) ||
        strlen(password) >= sizeof(s_mqtt_pass)) {
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open("zc_mqtt", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "uri", uri);
    if (err == ESP_OK) err = nvs_set_str(h, "user", username);
    if (err == ESP_OK) err = nvs_set_str(h, "mpass", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;
    strlcpy(s_uri, uri, sizeof(s_uri));
    strlcpy(s_user, username, sizeof(s_user));
    strlcpy(s_mqtt_pass, password, sizeof(s_mqtt_pass));
    if (s_client != NULL) {
        /* URI changed under a live client: restart it clean. */
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
    mqtt_start_client();
    return ESP_OK;
}

bool app_mqtt_configured(void)
{
    return s_ssid[0] != 0 && s_uri[0] != 0;
}

bool app_mqtt_owns_wifi(void)
{
    return true;
}

static int mqtt_wifi_cmd(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: mqtt-wifi <ssid> <password>\n");
        return 1;
    }
    esp_err_t err = app_mqtt_set_wifi(argv[1], argv[2]);
    if (err != ESP_OK) {
        printf("wifi config not saved: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("wifi config saved — connecting\n");
    return 0;
}
static int mqtt_broker_cmd(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mqtt-broker <uri> [username] [password]  (e.g. mqtt://192.168.1.10)\n");
        return 1;
    }
    esp_err_t err = app_mqtt_set_broker(argv[1], argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "");
    if (err != ESP_OK) {
        printf("broker config not saved: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("broker config saved\n");
    return 0;
}

/* mqtt-setup: the guided version of the commands above. */
#define MQTT_SETUP_ANSWER_MS 120000   /* per question: a person may go and find a value */

static bool mqtt_setup_ask(const char *prompt, char *buf, size_t len, bool secret)
{
    for (;;) {
        esp_err_t err = app_console_read_line(prompt, buf, len, secret, MQTT_SETUP_ANSWER_MS);
        if (err == ESP_OK) return true;
        if (err == ESP_ERR_INVALID_SIZE) {
            printf("  too long — at most %u characters\n", (unsigned)(len - 1));
            continue;
        }
        printf("mqtt-setup: no answer — stopped. Values already saved are kept; run mqtt-setup again to finish.\n");
        return false;
    }
}

static int mqtt_setup_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    char a[129], b[66], c[66], prompt[200];

    printf("mqtt-setup — answer each question; Enter keeps the value in [brackets].\n");
    snprintf(prompt, sizeof(prompt), "Wi-Fi network (SSID) [%s]: ", s_ssid);
    if (!mqtt_setup_ask(prompt, a, sizeof(s_ssid), false)) return 1;
    bool keep_ssid = a[0] == 0;
    if (keep_ssid && s_ssid[0] == 0) {
        printf("  skipped — no network set yet\n");
    } else {
        if (!mqtt_setup_ask(keep_ssid ? "Wi-Fi password [keep]: " : "Wi-Fi password: ", b, sizeof(s_pass), true)) return 1;
        if (!(keep_ssid && b[0] == 0)) {
            /* The setter copies into the live config: never pass it the live
             * config itself as the source. */
            if (keep_ssid) strlcpy(a, s_ssid, sizeof(a));
            esp_err_t err = app_mqtt_set_wifi(a, b);
            if (err != ESP_OK) printf("  not saved: %s\n", esp_err_to_name(err));
            else printf("  wifi config saved — connecting\n");
        }
    }

    snprintf(prompt, sizeof(prompt), "Broker URI, e.g. mqtt://192.168.1.10 [%s]: ", s_uri);
    if (!mqtt_setup_ask(prompt, a, sizeof(s_uri), false)) return 1;
    if (a[0] == 0 && s_uri[0] == 0) {
        printf("  skipped — no broker set yet\n");
    } else {
        /* Enter keeps the stored value, "-" clears it: a blank answer cannot
         * mean both, and moving to an anonymous broker has to be possible. */
        snprintf(prompt, sizeof(prompt), "Broker username (Enter keeps, - for none) [%s]: ", s_user);
        if (!mqtt_setup_ask(prompt, b, sizeof(s_user), false)) return 1;
        if (!mqtt_setup_ask("Broker password (Enter keeps, - for none): ", c, sizeof(s_mqtt_pass), true)) return 1;
        if (a[0] || b[0] || c[0]) {
            if (!a[0]) strlcpy(a, s_uri, sizeof(a));
            if (strcmp(b, "-") == 0) b[0] = 0;
            else if (!b[0]) strlcpy(b, s_user, sizeof(b));
            if (strcmp(c, "-") == 0) c[0] = 0;
            else if (!c[0]) strlcpy(c, s_mqtt_pass, sizeof(c));
            esp_err_t err = app_mqtt_set_broker(a, b, c);
            if (err != ESP_OK) printf("  not saved: %s\n", esp_err_to_name(err));
            else printf("  broker config saved\n");
        }
    }

    if (app_mqtt_configured()) {
        printf("mqtt-setup: done — connecting to %s.\n", s_uri);
    } else {
        printf("mqtt-setup: saved, but still missing:%s%s — run mqtt-setup again to finish.\n",
               s_ssid[0] ? "" : " Wi-Fi", s_uri[0] ? "" : " broker");
    }
    return 0;
}

esp_err_t app_mqtt_init(void)
{
    s_handle = app_driver_register_solution("mqtt", mqtt_driver_cb, NULL);

    ESP_ERROR_CHECK(esp_netif_init());
    /* Tolerant: aws_iot (and any behavior) may have created the default loop
     * already, and a second create is ESP_ERR_INVALID_STATE, not a failure. */
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_err);
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node_id, sizeof(s_node_id), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_status_topic, sizeof(s_status_topic), "zerocode/%s/status", s_node_id);

    /* Node-level contributions (behaviors) — before anything connects. */


    esp_console_cmd_t wifi_cmd = {};
    wifi_cmd.command = "mqtt-wifi";
    wifi_cmd.help = "Set Wi-Fi credentials: mqtt-wifi <ssid> <password>";
    wifi_cmd.func = &mqtt_wifi_cmd;
    app_console_register_cmd(&wifi_cmd);
    esp_console_cmd_t broker_cmd = {};
    broker_cmd.command = "mqtt-broker";
    broker_cmd.help = "Set broker: mqtt-broker <uri> [username] [password]";
    broker_cmd.func = &mqtt_broker_cmd;
    app_console_register_cmd(&broker_cmd);
    esp_console_cmd_t setup_cmd = {};
    setup_cmd.command = "mqtt-setup";
    setup_cmd.help = "Guided setup: asks for Wi-Fi, then the broker URI and its credentials";
    setup_cmd.func = &mqtt_setup_cmd;
    app_console_register_cmd(&setup_cmd);

    mqtt_load_config();
    if (s_ssid[0] == 0 || s_uri[0] == 0) {
        /* Unconfigured is a valid state — never block boot. */
        ESP_LOGW(TAG, "MQTT not configured — run: mqtt-setup (guided), or mqtt-wifi <ssid> <pass>, then mqtt-broker <uri>");
    }
    mqtt_wifi_start();

    ESP_LOGI(TAG, "MQTT initialized (node %s)", s_node_id);
    return ESP_OK;
}
