/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * ESP ZeroCode AI - MQTT (Home Assistant) Framework
 *
 * BASE: No-op stub. The generator replaces app_mqtt.cpp with the real
 * implementation when the product has device-type bindings.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>
#include <mqtt_client.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_mqtt_init(void);

/* ── Configuration ──────────────────────────────────────────────────────
 *
 * What the mqtt-* console commands do, as functions — for a product that
 * onboards some other way. Each validates, persists to NVS and applies at once
 * (a broker change restarts the client). The NVS layout stays this block's
 * business. The block already ships a guided `mqtt-setup` command; a product
 * wanting a different flow writes a console command over these setters and
 * app_console_read_line (app_console.h), not a second copy of the storage.
 */

/** Wi-Fi network. ESP_ERR_NOT_SUPPORTED when Matter/RainMaker owns Wi-Fi in
 *  this product (it is onboarded through them); ESP_ERR_INVALID_SIZE when the
 *  SSID is over 32 bytes or the password over 64. */
esp_err_t app_mqtt_set_wifi(const char *ssid, const char *password);

/** Broker, e.g. "mqtt://192.168.1.10". Username / password may be NULL or ""
 *  for none. URI over 128 bytes, username over 32 or password over 64:
 *  ESP_ERR_INVALID_SIZE. Restarts the MQTT client, so never call it from an
 *  MQTT event handler (it runs on the client's own task, which the restart
 *  stops) — call it from a console command or an app task. */
esp_err_t app_mqtt_set_broker(const char *uri, const char *username, const char *password);

/** Everything needed to connect is stored (Wi-Fi when this block owns it, and
 *  the broker). */
bool app_mqtt_configured(void);

/* The stored broker settings, read-only — for a settings page that shows them
 * without keeping a second copy. Never returns NULL; empty before setup. */
const char *app_mqtt_broker_uri(void);
const char *app_mqtt_user(void);
const char *app_mqtt_user_password(void);

/* TLS for the broker. The household's server answers on mqtts:// and presents
 * a VALID certificate issued for a different name; the session therefore runs
 * over TLS with the certificate's NAME check skipped (the chain is still
 * checked against the built-in pack unless a CA is supplied below).
 */

/** Whether the configured URI runs MQTT over TLS (mqtts://, ssl://, :8883). */
bool app_mqtt_broker_tls(void);

/** Optional root CA (PEM) for the broker's chain; NULL or "" clears it and the
 *  built-in certificate pack is used. Copied. */
void app_mqtt_set_broker_ca(const char *pem);
const char *app_mqtt_broker_ca(void);

/** Whether this block owns Wi-Fi (false when Matter/RainMaker is co-selected). */
bool app_mqtt_owns_wifi(void);

/* ── Raw session, for a product that speaks its own protocol ─────────────
 *
 * The session above is driven entirely by this block (Wi-Fi, broker, the
 * client's task). These give the product the handle to publish and subscribe
 * on it, without opening a second connection or a second client task.
 */

/** The client handle, or NULL before the broker config makes one. */
esp_mqtt_client_handle_t app_mqtt_client(void);

/** Subscribe to @p topic on the live session (nothing happens while offline;
 *  call it again when connected). The product's own data handler receives
 *  every message that arrives. */
int app_mqtt_subscribe(const char *topic);

/** Publish @p data on @p topic. -1 when there is no session. */
int app_mqtt_publish(const char *topic, const char *data, int len, int qos, int retain);

/** Install the handler that receives EVERY MQTT_EVENT_DATA — (topic,
 *  topic_len, data, data_len) — whatever topic the client forwards. Call it
 *  before app_mqtt_init(). */
void app_mqtt_set_data_handler(void (*handler)(const char *topic, int topic_len,
                                               const char *data, int data_len));

/** Tell the block that the application's own session is up (true) or gone
 *  (false), so it can hold the session or re-attempt the connection. */
void app_mqtt_set_online(bool online);

/** How long the transport keeps retrying a broker that never came up.
 *  @p delay_s is how often the panel re-attempts on its own (0 = mqtt's own
 *  back-off). A session that HAS come up once always retries indefinitely. */
void app_mqtt_set_offline_policy(bool retry_forever, uint32_t delay_s);

/** The broker session is up right now. */
bool app_mqtt_session_up(void);

/* ── SoftAP onboarding ───────────────────────────────────────────────────
 *
 * While no Wi-Fi credentials are stored the block serves its own access point
 * so the settings page can be reached on a factory-fresh panel. Saving a
 * network through app_mqtt_set_wifi() withdraws it.
 */

/** Whether any Wi-Fi network is stored. */
bool app_mqtt_wifi_missing(void);

/** Start (or keep alive) the onboarding access point. */
void app_mqtt_ap_start(void);

/** The access point's SSID (its fixed WPA2 password is in the boot log). */
void app_mqtt_ap_ssid(char *buf, size_t len);

bool app_mqtt_ap_active(void);

#ifdef __cplusplus
}
#endif
