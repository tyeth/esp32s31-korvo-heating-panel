/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The heating model: four rooms, their zigbee2mqtt topics, the live state the
 * dashboard shows, and the one-shot voice vocabulary.
 *
 * Everything the panel does — the LVGL dashboard, the touch controls, the voice
 * commands, the settings page and the MQTT plumbing — goes through this header.
 * Nothing here talks MQTT directly; publishes happen over the transport that
 * calls heating_broker_ready().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HEATING_ROOMS        4
#define HEATING_NAME_MAX     16
#define HEATING_TOPIC_MAX    96

/* 0.1 °C units throughout: 210 = 21.0 °C. */
#define HEATING_MIN_TENTH    50    /* 5.0 °C  */
#define HEATING_MAX_TENTH    300   /* 30.0 °C */
#define HEATING_STEP_TENTH   5     /* 0.5 °C  */

/* A value not refreshed for this long is greyed and stamped with its time. */
#define HEATING_STALE_S      600
/* The same target twice within this window is refused ("Already set"). */
#define HEATING_DUP_WINDOW_S 30

typedef struct {
    char     name[HEATING_NAME_MAX];
    char     trv_topic[HEATING_TOPIC_MAX];    /* also the setpoint publish topic */
    char     sensor_topic[HEATING_TOPIC_MAX]; /* room air sensor, may be empty */
} heating_room_cfg_t;

typedef enum {
    HEATING_HEATING = 0,
    HEATING_MODE_OFF,
} heating_mode_t;

typedef struct {
    bool     valid;
    int16_t  tenth;             /* 0.1 °C */
    int64_t  seen_ms;
} heating_reading_t;

typedef struct {
    heating_room_cfg_t cfg;
    heating_reading_t  target;   /* current_heating_setpoint (reported by the TRV) */
    heating_reading_t  valve;    /* local_temperature (the TRV's own probe) */
    heating_reading_t  room;     /* the room air sensor */
    heating_mode_t     mode;
    int16_t            read_target;     /* target the card shows (0.1 °C) */
    int16_t            last_pub_tenth;  /* last setpoint this panel published */
    int64_t            last_pub_ms;
} heating_room_t;

/* ── model ───────────────────────────────────────────────────────────── */

/** Load the saved room table and the last published targets. */
esp_err_t heating_init(void);

heating_room_t *heating_room(int idx);

/** Current room temperature: the air sensor when it is fresher than the TRV's
 *  own probe, else local_temperature. false when neither has reported. */
bool heating_room_current(const heating_room_t *room, int16_t *tenth, bool *stale, int64_t *seen_ms);

/** Local time of a reading for the "last seen" stamp; false before SNTP. */
bool heating_format_time(int64_t seen_ms, char *buf, size_t len);

/** Setpoint the UI starts from at boot: the last one this panel published,
 *  or 20.0 °C before the first one. */
int16_t heating_room_target_ui(const heating_room_t *room);

/** How many rooms have a reading younger than HEATING_STALE_S. */
int heating_rooms_reporting(void);

/** The zigbee2mqtt COMMAND topic a room's setpoint is published to (the state
 *  topic is what is subscribed; the two differ). Never NULL. */
const char *heating_room_command_topic(int idx);

/** Restart the staleness clock for every reading (used on a transport change). */
void heating_mark_all_stale(void);

/** The setpoint last published to each TRV, before the first connect uses it
 *  as that room's card target. */
void heating_load_targets(void);

/* ── setpoints ───────────────────────────────────────────────────────── */

typedef enum {
    HEATING_REQ_OK = 0,
    HEATING_REQ_OFFLINE,
    HEATING_REQ_ALREADY_SET,
    HEATING_REQ_NO_ROOM,
} heating_req_result_t;

/** True when the setpoint topic can be published right now. */
bool heating_online(void);

/** Ask for a room target (0.1 °C): clamps to 5–30 °C, rounds to 0.5 °C,
 *  writes the TRV's state topic and remembers it for the duplicate window.
 *  Refused — never queued — while offline. */
heating_req_result_t heating_request_target(int idx, int16_t tenth);

/** The target actually published for @p tenth: 5–30 °C in 0.5 °C steps. */
int16_t heating_clamp_tenth(int16_t tenth);

/** Publish {"time": 254} to the zigbee2mqtt permit-join request topic. */
esp_err_t heating_permit_join(void);

/* ── log verbosity ─────────────────────────────────────────────────────
 *
 * One selection for the whole console, kept in NVS so it survives a reboot:
 * the default level for every tag the panel does not name, plus the driver tags
 * that would otherwise fill the console (MSPI, ADC).
 * It is the setting the settings page offers and the `log` command reads.
 * sdkconfig's VERBOSE default still applies until one is chosen here. */

/** Apply the saved selection (or the build default when none was saved).
 *  Called at boot, before the board manager starts logging. */
void heating_log_level_apply_boot(void);

/** The stored verbosity name: "ERROR", "WARN", "INFO", "DEBUG" or
 *  "VERBOSE" (the default when nothing was saved). */
const char *heating_log_level_get(void);

/** Store and apply a verbosity (one of the names above). */
esp_err_t heating_log_level_set(const char *name);


/** Ask every configured room topic (TRV and air sensor) for its current state:
 *  zigbee2mqtt only publishes on change, so this is what fills the cards at
 *  boot instead of waiting for a valve to move. Returns how many topics were
 *  asked. */
int heating_request_states(void);

/* ── inbound ─────────────────────────────────────────────────────────── */

/** Feed one state message received on @p topic. */
void heating_on_mqtt_message(const char *topic, int topic_len, const char *data, int data_len);

/** The transport's session state. */
void heating_broker_ready(bool online);

/* ── voice ───────────────────────────────────────────────────────────── */

/**
 * The voice vocabulary. heating_voice_register() runs from the audio
 * framework's command-registration slot — between its esp_mn_commands_clear()
 * and esp_mn_commands_update(), inside app_audio_init() — and
 * heating_voice_apply_command() / heating_voice_on_event() are the audio
 * framework's command and listening-state hooks.
 */
esp_err_t heating_voice_register(void);
void heating_voice_apply_command(int phrase_id);
/** Log the exact word list the recogniser holds (called as the window opens, so
 *  a mis-said or missing phrase can be read straight off the console). */
void heating_voice_print_vocabulary(void);
/** Relative commands ("warm up" / "cool down") act on the selected room. */
void heating_voice_set_selected_room(int idx);
/** What the panel currently offers (for a button that opens the window). */
bool heating_ui_has_touch(void);
/** A listening window that closed without a recognised command. */
void heating_voice_not_understood(void);

/** Microphone window opened / closed (amber LED, "Listening…" bar). */
void heating_ui_listening(bool listening);
/** The same transition for the dashboard alone (the strip has its own). */
void heating_ui_listening_ui(bool listening);

/** @p listening_s seconds left in the open window, for the countdown bar. */
void heating_voice_seconds_left(int seconds);

/* ── display hooks the dashboard installs ────────────────────────────── */

/** Something the dashboard renders has changed. */
void heating_ui_invalidate(void);
/** The transport's session state changed (the strip and the footer follow it). */
void heating_ui_sync_online(void);
/** Make room @p idx the active card (the one −/+ act on). */
void heating_ui_select(int idx);
/** Show a transient banner for a few seconds. */
void heating_ui_banner(const char *text);
/** A command was refused: flash the strip red for a second. */
void heating_ui_rejected(void);
/** The same for the dashboard alone (the strip has its own). */
void heating_ui_rejected_ui(void);

/** The panel's screen and touch panel (the board's own LCD and GT1151). */
esp_err_t heating_ui_init(void);
/** A press anywhere (touch or button) restores full brightness. */
void heating_ui_touch_activity(void);
/** Product activity worth waking the screen for (a heard command, a setpoint). */
void heating_ui_poke(void);

/** MQTT: the product's own state handler and transport glue. */
void heating_mqtt_install(void);
/** The bridge session came up (true) or went away (false). */
void heating_broker_ready(bool online);
/** The Wi-Fi radio was started: the settings page can be reached. */
void heating_web_announce(void);

/** The settings page on port 80 (Wi-Fi, broker, room topics). */
void heating_web_install(void);

/** The voice listening-window transitions from the audio framework. */
void heating_voice_on_event(int listening);

/* ── persistence ─────────────────────────────────────────────────────── */

/** Save the room table and every remembered target. */
esp_err_t heating_save_cfg(void);

/** Re-read the saved room table after heating_save_cfg() changed it. */
void heating_reload_cfg(void);

/** Wipe the saved Wi-Fi/broker/room settings and reboot into onboarding. */
void heating_factory_reset(void);

/** Log everything the panel knows about its link and its topics, at INFO: the
 *  MQTT config, the session, and every room's state and command topic. Called
 *  once per broker session and by the console `mqtt-diag`. */
void heating_log_diag(void);

#ifdef __cplusplus
}
#endif
