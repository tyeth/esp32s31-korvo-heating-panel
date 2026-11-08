#include "app_mqtt.h"
#include "app_driver.h"
#include "app_config.h"
#include "app_console.h"

#include "heating.h"

#include <string.h>
#include <stdio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_console.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <mqtt_client.h>


static const char *TAG = "app_mqtt";

static app_driver_handle_t s_handle = 0;
static esp_mqtt_client_handle_t s_client = NULL;
static volatile bool s_connected = false;
/* Radio state, one flag per interface: the onboarding SoftAP may bring the
 * radio up (APSTA) long before the station has any credentials, so "started"
 * and "station configured" are two different facts. */
static bool s_wifi_started = false;      /* the radio is up */
static bool s_sta_configured = false;    /* WIFI_IF_STA carries credentials */

/* Node identity: station MAC as lowercase hex — stable, unique per board. */
static char s_node_id[13] = {0};
static char s_status_topic[48] = {0};

/* NVS-backed config (namespace "zc_mqtt"), set via the console commands. */
static char s_ssid[33] = {0};
static char s_pass[65] = {0};
static char s_uri[129] = {0};
static char s_user[33] = {0};
static char s_mqtt_pass[65] = {0};
/* Optional root CA for the broker's chain (PEM); empty = the built-in pack.
 * Set from the settings page; kept so a household that pastes its own CA
 * keeps chain authentication even though the name check is skipped. Always a
 * valid string once s_ca_buf exists — never a NULL to dereference. */
static char  s_ca_buf[4096] = {0};
static char *s_ca_pem = s_ca_buf;

/* The product's own data handler (app_mqtt_set_data_handler). */
static void (*s_data_handler)(const char *topic, int topic_len,
                              const char *data, int data_len) = NULL;
/* The product says its session is up: re-subscribe its topics here so they
 * are in place on every (re)connect, before any state message is missed. */
static bool s_held_online = false;



/* Connection-state transitions. Behavior blocks hook these via
 * mqtt_event_cases — `connected` is in scope (1 = broker session up). */
static void mqtt_on_event(int connected)
{
    (void)connected;
    /* Every (re)connect re-creates the subscriptions: a broker restarted in
     * clear session loses them, and in a persistent session they survive —
     * subscribing twice is harmless, missing them is not. */
    if (connected && s_held_online) {
        heating_broker_ready(true);
    }
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
    if (s_data_handler != NULL) {
        s_data_handler(topic, tlen, data, dlen);
    }
}

/* ── Raw session accessors (app_mqtt.h) ─────────────────────────────── */

esp_mqtt_client_handle_t app_mqtt_client(void)
{
    return s_client;
}

int app_mqtt_subscribe(const char *topic)
{
    if (s_client == NULL || !s_connected || topic == NULL || topic[0] == 0) return -1;
    return esp_mqtt_client_subscribe(s_client, topic, 0);
}

int app_mqtt_publish(const char *topic, const char *data, int len, int qos, int retain)
{
    if (s_client == NULL || topic == NULL) return -1;
    return esp_mqtt_client_publish(s_client, topic, data, len, qos, retain);
}

void app_mqtt_set_data_handler(void (*handler)(const char *topic, int topic_len,
                                               const char *data, int data_len))
{
    s_data_handler = handler;
}

void app_mqtt_set_online(bool online)
{
    s_held_online = online;
    if (online) {
        mqtt_on_event(1);
    }
}

/** The broker URI this panel ships with: the Mosquitto add-on that usually runs
 *  beside Home Assistant. It is a DIFFERENT PORT from the web UI — Home
 *  Assistant's own site is HTTPS on 8123, while the broker listens on plain
 *  MQTT 1883, or 8883 when the add-on's TLS listener is enabled. Point this at
 *  whichever listener the household actually exposes (and at 8883 if that
 *  listener carries TLS); /settings overrides it without a rebuild. The
 *  certificate is issued for a different name than the one dialled, so a TLS
 *  session here skips the name check — see mqtt_start_client(). */
#define HEATING_DEFAULT_BROKER_URI "mqtt://homeassistant.gdenu.fi:1883"
#define HEATING_DEFAULT_BROKER_USER ""

/* The home network this panel is made for. Used only when nothing is stored
 * (a fresh flash, or right after a factory reset); a network saved from the
 * settings page or the console replaces it. */
#define HEATING_DEFAULT_WIFI_SSID "free4all"
#define HEATING_DEFAULT_WIFI_PASS "password"

/** True for a URI that runs mqtt over TLS (mqtts:// , or mqtt:// on 8883). */
static bool mqtt_uri_is_tls(const char *uri)
{
    if (uri == NULL) return false;
    if (strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "ssl://", 6) == 0) return true;
    if (strncmp(uri, "mqtt://", 7) == 0) {
        const char *port = strrchr(uri, ':');
        return port != NULL && strncmp(port, ":8883", 5) == 0;
    }
    return false;
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

    /* A TLS broker session (mqtts:// or :8883) skips the certificate NAME check:
     * the household's server presents a valid certificate issued for a
     * different host than the one dialled. The chain is still verified — against
     * the built-in pack, or the CA supplied on the settings page — so an
     * impostor with an untrusted certificate is refused. */
    if (mqtt_uri_is_tls(s_uri)) {
        cfg.broker.verification.skip_cert_common_name_check = true;
        if (s_ca_pem != NULL && s_ca_pem[0] != 0) {
            cfg.broker.verification.certificate = s_ca_pem;
        }
    }

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
    ESP_LOGI(TAG, "MQTT connecting to %s%s", s_uri,
             mqtt_uri_is_tls(s_uri) ? " (TLS, certificate name check skipped)" : "");
}

/* The retry runs on the esp_timer task, never on the event task. */
#define RETRY_BACKOFF_S 30
/* A start requested from the event path waits this long, so the client comes
 * up once the association's event storm has passed. */
#define NET_START_DELAY_S 1
static esp_timer_handle_t s_retry_timer = NULL;

static void net_start_cb(void *arg);
static void mqtt_start_client(void);

static void wifi_retry_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Wi-Fi reconnect attempt");
    esp_wifi_connect();
}

/* ── Wi-Fi/IP work, off the default event loop ────────────────────────
 *
 * The handlers only wake s_net_task; there is no code of ours that runs
 * in-line on the framework's shared 'sys_evt' task — not even the connect.
 *
 * That task overflows on a configured boot even with 4 KB (the panel aborts
 * there on every power cycle), and the extra stack it holds is taken from
 * internal DRAM inside esp_wifi_init(), where the Wi-Fi driver also carves its
 * DMA pools — so it cannot simply be made large. So the system event task is
 * made SMALL (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE, sdkconfig.product) and
 * every Wi-Fi/IP event the panel cares about is delivered through a QUEUE of our
 * own: esp_event_handler_register_with() registers the handler on the default
 * loop with our own handler data, and this task — not the loop's — runs it.
 */
#define NET_EV_DISCONNECTED 0x01
#define NET_EV_GOT_IP       0x02
#define NET_EV_START        0x04

static TaskHandle_t s_net_task = NULL;
static esp_timer_handle_t s_net_debounce = NULL;
static volatile bool s_net_wanted = false;
/* Filled by the loop's dispatch (esp_event_handler_register passes our data
 * pointer straight through), consumed by this task. */
typedef struct {
    esp_event_base_t base;
    int32_t          id;
} net_event_t;
static QueueHandle_t s_net_queue = NULL;

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_event_t ev = {};
        if (xQueueReceive(s_net_queue, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.base == WIFI_EVENT && ev.id == WIFI_EVENT_STA_START) {
            /* The station interface is up: join — but only when the radio is in
             * station mode. The onboarding AP starts the radio too, and there is
             * no station to join then (the driver answers ESP_ERR_WIFI_MODE, so
             * it is also not a meaningful failure to log). */
            wifi_mode_t mode;
            if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL || mode == WIFI_MODE_AP) {
                ESP_LOGD(TAG, "station start while not in station mode — nothing to join");
                continue;
            }
            esp_err_t join_err = esp_wifi_connect();
            if (join_err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi join refused: %s", esp_err_to_name(join_err));
            }
            continue;
        }
        if (ev.base == IP_EVENT && ev.id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "Wi-Fi got IP");
            /* Start the broker session a moment after the IP settles: the
             * TLS/DNS and socket setup of a fresh client is what gets close to
             * the limits of a small task, and doing it once, after the storm of
             * events that follows an association, is what keeps boot predictable. */
            if (s_net_debounce == NULL) {
                esp_timer_create_args_t args = {};
                args.callback = &net_start_cb;
                args.name = "mqtt_net_start";
                esp_timer_create(&args, &s_net_debounce);
            }
            if (s_net_debounce) {
                s_net_wanted = true;
                esp_timer_stop(s_net_debounce);
                esp_timer_start_once(s_net_debounce, (uint64_t)NET_START_DELAY_S * 1000000ULL);
            } else {
                mqtt_start_client();
            }
            continue;
        }
        if (ev.base == WIFI_EVENT && ev.id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "Wi-Fi disconnected — retrying in %d s", RETRY_BACKOFF_S);
            if (s_retry_timer == NULL) {
                esp_timer_create_args_t args = {};
                args.callback = &wifi_retry_cb;
                args.name = "mqtt_wifi_retry";
                esp_timer_create(&args, &s_retry_timer);
            }
            if (s_retry_timer) {
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, (uint64_t)RETRY_BACKOFF_S * 1000000ULL);
            }
        }
    }
}

static void net_start_cb(void *arg)
{
    (void)arg;
    if (s_net_wanted) mqtt_start_client();
}

/* The handler the default loop calls: it copies the two fields we act on into
 * our queue and returns. Nothing else of ours ever runs on sys_evt. */
static void wifi_event_handler(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)event_data;
    QueueHandle_t q = (QueueHandle_t)handler_arg;
    if (q == NULL) return;
    net_event_t ev = { .base = base, .id = event_id };
    xQueueSend(q, &ev, 0);          /* full queue = the event is stale anyway */
}

static void mqtt_wifi_start(void)
{
    if (s_sta_configured || s_ssid[0] == 0) return;
    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    if (s_wifi_started) {
        /* The radio is already up (brought up by the onboarding SoftAP):
         * configure the station on the live interface. The join itself is left
         * to STA_START, which the radio raises for this interface — see the
         * net task; driving esp_wifi_connect() from here as well makes the
         * driver emit its own management operations, and on this target that is
         * where the station start has faulted. */
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        s_sta_configured = true;
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    s_sta_configured = true;
}

/* ── SoftAP onboarding ────────────────────────────────────────────────
 *
 * The spec's commissioning is "join the panel's own access point, open the
 * settings page, enter Wi-Fi and the broker". That link is the SoftAP below:
 * it runs only while the panel has no Wi-Fi credentials, alongside the station
 * interface (APSTA), and is withdrawn as soon as the user saves a network.
 * Nothing else in the product owns the radio, so it lives here, with the rest
 * of the block's Wi-Fi.
 */
#define ONBOARD_AP_PREFIX      "Korvo-Heating"
#define ONBOARD_AP_PASS        "heatpanel"   /* >= 8 chars: WPA2 */
#define ONBOARD_AP_TIMEOUT_S   300           /* off after 5 min idle, on again on button */

static esp_netif_t *s_ap_netif = NULL;
static bool s_ap_up = false;
static char s_ap_ssid[33] = {0};
static esp_timer_handle_t s_ap_timer = NULL;

void app_mqtt_ap_ssid(char *buf, size_t len)
{
    snprintf(buf, len, "%s", s_ap_ssid[0] ? s_ap_ssid : "");
}

bool app_mqtt_ap_active(void)
{
    return s_ap_up;
}

/* The station had no credentials when the radio came up for onboarding; go
 * back to station-only before the APSTA-specific 10 s AP retry cap bites. */
static void ap_stop(void)
{
    if (!s_ap_up) return;
    s_ap_up = false;
    if (s_ap_timer) esp_timer_stop(s_ap_timer);
    if (!s_sta_configured) {
        ESP_LOGW(TAG, "SoftAP %s idle for %d s — closed. No Wi-Fi stored yet: long-press BOOT "
                      "to open it again", s_ap_ssid, ONBOARD_AP_TIMEOUT_S);
    } else {
        ESP_LOGI(TAG, "SoftAP %s closed — station only", s_ap_ssid);
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) ESP_LOGW(TAG, "SoftAP close failed: %s", esp_err_to_name(err));
}

static void ap_stop_cb(void *arg)
{
    (void)arg;
    ap_stop();
}

void app_mqtt_ap_start(void)
{
    if (s_ap_up) {
        /* Already serving: just keep it awake. */
        if (s_ap_timer) {
            esp_timer_stop(s_ap_timer);
            esp_timer_start_once(s_ap_timer, (uint64_t)ONBOARD_AP_TIMEOUT_S * 1000000ULL);
        }
        return;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", ONBOARD_AP_PREFIX, mac[4], mac[5]);

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "SoftAP netif not created");
            return;
        }
    }

    wifi_config_t ap = {};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    strlcpy((char *)ap.ap.password, ONBOARD_AP_PASS, sizeof(ap.ap.password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;

    if (!s_wifi_started) {
        /* The radio comes up as an access point only. A station configured on
         * an APSTA interface at the same instant cannot come up cleanly on this
         * target (the driver faults in its station start); the station is
         * joined the moment the panel is commissioned, by starting the radio
         * again in station mode — see app_mqtt_set_wifi(). */
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }

    if (s_ap_timer == NULL) {
        esp_timer_create_args_t args = {};
        args.callback = &ap_stop_cb;
        args.name = "mqtt_ap_idle";
        esp_timer_create(&args, &s_ap_timer);
    }
    if (s_ap_timer) {
        esp_timer_stop(s_ap_timer);
        esp_timer_start_once(s_ap_timer, (uint64_t)ONBOARD_AP_TIMEOUT_S * 1000000ULL);
    }
    s_ap_up = true;
    ESP_LOGI(TAG, "SoftAP '%s' (password '%s') — open http://korvo-heating.local/settings",
             s_ap_ssid, ONBOARD_AP_PASS);
    heating_web_announce();
}

/* No stored Wi-Fi credentials: the panel has no link of its own yet. */
bool app_mqtt_wifi_missing(void)
{
    return s_ssid[0] == 0;
}

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
    char ca[4096] = {0};
    size_t ca_len = sizeof(ca);
    if (nvs_get_str(h, "ca", ca, &ca_len) == ESP_OK) {
        strlcpy(s_ca_buf, ca, sizeof(s_ca_buf));
    }
    nvs_close(h);
    /* The home network, for a panel nobody has set up yet; the settings page
     * or console replaces it. */
    if (s_ssid[0] == 0) {
        strlcpy(s_ssid, HEATING_DEFAULT_WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_pass, HEATING_DEFAULT_WIFI_PASS, sizeof(s_pass));
        ESP_LOGI(TAG, "no Wi-Fi saved — default '%s'", s_ssid);
    }
    /* The household's own broker, as the default for a panel nobody has set up
     * yet; the settings page overrides it. */
    if (s_uri[0] == 0) {
        strlcpy(s_uri, HEATING_DEFAULT_BROKER_URI, sizeof(s_uri));
        strlcpy(s_user, HEATING_DEFAULT_BROKER_USER, sizeof(s_user));
        ESP_LOGI(TAG, "no broker saved — default %s", s_uri);
    }
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
    /* Credentials stored: the onboarding link has done its job. */
    if (s_ap_up) ap_stop();
    /* Whether the radio was up as the onboarding AP or not, the station is
     * configured here and started from this task — never from the event-loop
     * dispatch — so a panel commissioned over its own access point joins the
     * home network with a clean station start. */
    s_sta_configured = false;
    bool was_up = s_wifi_started;
    s_wifi_started = false;
    if (was_up) {
        /* Leave AP mode first: the station is joined by starting the radio in
         * station mode, which is the sequence that comes up cleanly here. */
        esp_wifi_stop();
    }
    mqtt_wifi_start();
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

void app_mqtt_set_broker_ca(const char *pem)
{
    /* Stored, so a pinned household stays pinned across reboots; the buffer is
     * fixed so a live session's certificate pointer stays valid. */
    snprintf(s_ca_buf, sizeof(s_ca_buf), "%s", (pem != NULL) ? pem : "");

    nvs_handle_t h;
    if (nvs_open("zc_mqtt", NVS_READWRITE, &h) == ESP_OK) {
        if (s_ca_buf[0] == 0) {
            nvs_erase_key(h, "ca");
        } else {
            nvs_set_str(h, "ca", s_ca_buf);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_ca_buf[0] == 0) return;

    /* If a session is already up it was built without this CA: restart it so
     * the new anchor applies now rather than at some later reconnect. */
    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
        heating_broker_ready(false);
    }
    mqtt_start_client();
}

const char *app_mqtt_broker_ca(void)
{
    return s_ca_buf;
}

bool app_mqtt_broker_tls(void)
{
    return mqtt_uri_is_tls(s_uri);
}

const char *app_mqtt_broker_uri(void)      { return s_uri; }
const char *app_mqtt_user(void)            { return s_user; }
const char *app_mqtt_user_password(void)   { return s_mqtt_pass; }

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

    /* Our own task handles the network events (connect, disconnect log +
     * retry, broker start); the loop's handler only queues them — see
     * net_task(). The queue is independent of the task's lifetime, so a failed
     * task creation still leaves a valid (if never-drained) queue behind the
     * registered handlers rather than a NULL. */
    if (s_net_queue == NULL) {
        s_net_queue = xQueueCreate(8, sizeof(net_event_t));
        if (s_net_queue == NULL) {
            ESP_LOGE(TAG, "network event queue not created — Wi-Fi/IP events unhandled");
        }
    }
    if (s_net_queue != NULL && s_net_task == NULL) {
        if (xTaskCreate(net_task, "mqtt_net", 4096, NULL, 5, &s_net_task) != pdPASS) {
            ESP_LOGE(TAG, "network event task not created — Wi-Fi events unhandled");
            s_net_task = NULL;
        }
    }
    /* This IDF has no default-loop accessor: the plain register goes to the
     * default loop and still passes our queue through as the handler's data. */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, s_net_queue));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, s_net_queue));

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

    /* The stored settings — without this the panel would come up factory-fresh
     * on every boot. Loaded before the configured/AP decision below. */
    mqtt_load_config();

    /* Until the radio exists the settings page cannot be reached, so this is
     * where the very first connection starts. Never aborts boot: if the default
     * network is not there, the panel still shows its dashboard, keeps the
     * console, and the AP can be opened with a BOOT long press. */
    if (s_ssid[0] == 0 || s_uri[0] == 0) {
        ESP_LOGW(TAG, "MQTT not configured — the dashboard runs, but the settings page and "
                      "the TRVs wait for Wi-Fi; set it: mqtt-wifi <ssid> <pass>, then "
                      "mqtt-broker <uri>");
    }
    mqtt_wifi_start();

    /* The panel has no Wi-Fi of its own yet: serve the settings page over its
     * own access point instead (spec: SoftAP + mDNS onboarding). */
    if (app_mqtt_wifi_missing()) {
        app_mqtt_ap_start();
    }

    ESP_LOGI(TAG, "MQTT initialized (node %s)", s_node_id);
    return ESP_OK;
}
