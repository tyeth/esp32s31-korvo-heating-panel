/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * One-shot voice commands for the heating panel.
 *
 * The vocabulary is assembled at boot — it names the rooms the settings page
 * may have renamed — and registered with the voice framework as individual
 * phrases (the documented per-word API, not one fixed sdkconfig command set).
 * A recognised phrase id comes back here; the outcome lands on the screen
 * (there are no spoken replies).
 */

#include "heating.h"
#include "app_logic.h"
#include "app_config.h"

#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <esp_mn_speech_commands.h>

static const char *TAG = "heating_voice";

/* Phrase ids: four consecutive per room (a fixed target said four ways), then
 * the relative commands. One id range per room keeps the dispatch short. */
#define VOICE_ID_SET_BASE      1600
#define VOICE_ID_SET_VARIANTS  4
#define VOICE_ID_WARM_UP       (VOICE_ID_SET_BASE + HEATING_ROOMS * VOICE_ID_SET_VARIANTS)
#define VOICE_ID_COOL_DOWN     (VOICE_ID_WARM_UP + 1)
#define VOICE_ID_CANCEL        (VOICE_ID_WARM_UP + 2)

#define VOICE_MAX_PHRASES      (HEATING_ROOMS * VOICE_ID_SET_VARIANTS + 3)
#define VOICE_PHRASE_MAX       64

typedef struct {
    int16_t     id;
    const char *room;      /* the room's display name, NULL for the relative commands */
} voice_phrase_t;

static voice_phrase_t s_table[VOICE_MAX_PHRASES];
static int s_table_n = 0;
static bool s_registered = false;

/* The relative commands act on the room selected on screen. */
static int s_sel_room = 0;

void heating_voice_set_selected_room(int idx)
{
    if (idx >= 0 && idx < HEATING_ROOMS) s_sel_room = idx;
}

/* Phrase text for table entry @p i; @p buf must hold VOICE_PHRASE_MAX bytes. */
static void phrase_text(int i, char *buf, size_t len)
{
    static const char *const k_variants[VOICE_ID_SET_VARIANTS] = {
        "set the %s to 21",
        "set the %s to 21.5",
        "%s 21 and a half",
        "%s 22",
    };
    if (i < HEATING_ROOMS * VOICE_ID_SET_VARIANTS) {
        snprintf(buf, len, k_variants[i % VOICE_ID_SET_VARIANTS], s_table[i].room);
        return;
    }
    int rel = i - HEATING_ROOMS * VOICE_ID_SET_VARIANTS;
    snprintf(buf, len, "%s", rel == 0 ? "plus one" : (rel == 1 ? "minus one" : "cancel"));
}

static void build_table(void)
{
    int n = 0;
    for (int r = 0; r < HEATING_ROOMS; r++) {
        for (int v = 0; v < VOICE_ID_SET_VARIANTS; v++) {
            s_table[n].id = (int16_t)(VOICE_ID_SET_BASE + r * VOICE_ID_SET_VARIANTS + v);
            s_table[n].room = heating_room(r)->cfg.name;
            n++;
        }
    }
    s_table[n++] = { VOICE_ID_WARM_UP,   NULL };
    s_table[n++] = { VOICE_ID_COOL_DOWN, NULL };
    s_table[n++] = { VOICE_ID_CANCEL,    NULL };
    s_table_n = n;
}

/**
 * Register the vocabulary before the voice framework updates its model. Called
 * from the audio framework's command-registration slot; a no-op after the
 * first call so a failed voice start cannot leave the list half-built.
 */
esp_err_t heating_voice_register(void)
{
    if (s_registered) return ESP_OK;
    build_table();
    int added = 0;
    char phrase[VOICE_PHRASE_MAX];
    for (int i = 0; i < s_table_n; i++) {
        phrase_text(i, phrase, sizeof(phrase));
        if (esp_mn_commands_add(s_table[i].id, phrase) == ESP_OK) {
            added++;
        } else {
            ESP_LOGW(TAG, "phrase '%s' rejected by MultiNet", phrase);
        }
    }
    s_registered = true;
    ESP_LOGI(TAG, "%d of %d phrases registered", added, s_table_n);
    return ESP_OK;
}

/* ── applying an outcome ─────────────────────────────────────────────── */

static void apply(int idx, int16_t tenth)
{
    heating_room_t *room = heating_room(idx);
    if (room == NULL) return;
    heating_ui_select(idx);

    switch (heating_request_target(idx, tenth)) {
        case HEATING_REQ_OK: {
            char msg[64];
            int16_t t = heating_room_target_ui(room);
            snprintf(msg, sizeof(msg), "%s \xE2\x86\x92 %d.%d\xC2\xB0" "C",
                     room->cfg.name, (int)(t / 10), (int)((t % 10 + 10) % 10));
            heating_ui_banner(msg);
            break;
        }
        case HEATING_REQ_ALREADY_SET:
            heating_ui_banner("Already set");
            break;
        case HEATING_REQ_OFFLINE:
            heating_ui_banner("Offline \xE2\x80\x94 controls disabled");
            heating_ui_rejected();
            break;
        default:
            break;
    }
}

/* Called by the audio framework for one recognised phrase id. */
void heating_voice_apply_command(int phrase_id)
{
    if (!s_registered) return;                 /* voice came up without us */

    if (phrase_id == VOICE_ID_CANCEL) {
        heating_ui_banner("Cancelled");
        return;
    }
    if (phrase_id == VOICE_ID_WARM_UP || phrase_id == VOICE_ID_COOL_DOWN) {
        heating_room_t *room = heating_room(s_sel_room);
        if (room == NULL) return;
        int16_t step = (phrase_id == VOICE_ID_WARM_UP) ? 10 : -10;
        apply(s_sel_room, (int16_t)(heating_room_target_ui(room) + step));
        return;
    }
    int rel = phrase_id - VOICE_ID_SET_BASE;
    if (rel < 0 || rel >= HEATING_ROOMS * VOICE_ID_SET_VARIANTS) return;
    int room_idx = rel / VOICE_ID_SET_VARIANTS;
    static const int16_t k_tenth[VOICE_ID_SET_VARIANTS] = { 210, 215, 215, 220 };
    apply(room_idx, k_tenth[rel % VOICE_ID_SET_VARIANTS]);
}

/* Timeout with nothing recognised: the window closes, the screen says so. */
void heating_voice_not_understood(void)
{
    heating_ui_listening(false);
}

int heating_voice_vocabulary(int16_t *out, int max)
{
    (void)out; (void)max;
    return s_table_n;
}
