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
 * heating_voice_register() is called by app_audio_init() between its
 * esp_mn_commands_clear() and esp_mn_commands_update(), and a recognised id
 * comes back to heating_voice_apply_command().
 *
 * What is spoken and what happens:
 *
 *   "set the bathroom to 20"        bathroom  → 20.0 °C   (published now)
 *   "set the office to 21.5"        office    → 21.5 °C
 *   "hallway 22" / "living room 19" the same, without the "set the … to"
 *   "bathroom 21 and a half"        the same, said with a half
 *   "warm up" / "cool down"         the room selected on screen, ±1 °C
 *   "cancel"                        nothing is published
 *   "set the kitchen to 20"         "Kitchen has no smart TRV"
 *
 * English only, US English model (mn7_en): the numbers more than one aside are
 * spoken above / after the room, never as a bare digit.
 *
 * The outcome lands on the screen: there are no spoken replies.
 */

#include "heating.h"
#include "app_logic.h"
#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_mn_speech_commands.h>

static const char *TAG = "heating_voice";

/* Phrase ids: three per room (one per spoken number, see k_variants), then the
 * two rooms with no valve, then the relative commands. An id carries both what
 * to do and which number was said — the words after the room are what the user
 * actually said, not a digit recovered from text. */
#define VOICE_ID_SET_BASE      1600
#define VOICE_ID_SET_VARIANTS  3
#define VOICE_ID_NO_TRV_BASE   (VOICE_ID_SET_BASE + HEATING_ROOMS * VOICE_ID_SET_VARIANTS)
#define VOICE_ID_NO_TRV_COUNT  2
#define VOICE_ID_WARM_UP       (VOICE_ID_NO_TRV_BASE + VOICE_ID_NO_TRV_COUNT)
#define VOICE_ID_COOL_DOWN     (VOICE_ID_WARM_UP + 1)
#define VOICE_ID_CANCEL        (VOICE_ID_COOL_DOWN + 1)

#define VOICE_MAX_PHRASES      (HEATING_ROOMS * VOICE_ID_SET_VARIANTS + VOICE_ID_NO_TRV_COUNT + 3)
#define VOICE_PHRASE_MAX       64

/* The rooms that exist in the house but carry no smart TRV — naming one gets
 * "Kitchen has no smart TRV" instead of silence (spec failureModes). */
static const char *const k_unheated[VOICE_ID_NO_TRV_COUNT] = { "Kitchen", "Bedroom" };

/* The spoken numbers. Each phrase says the target it means, so the published
 * value is exactly what the user said; the three are the living-room values
 * from the spec's examples, said the three ways a person says them. */
static const int16_t k_variant_tenth[VOICE_ID_SET_VARIANTS] = {
    200,   /* "set the <room> to 20"        */
    215,   /* "set the <room> to 21.5"      */
    210,   /* "<room> 21 and a half"        */
};

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
    /* English, as a person says it: the room name in the middle, the number at
     * the end. The MultiNet model takes them and prints the recognised phrase
     * as English words in the boot log. */
    static const char *const k_variants[VOICE_ID_SET_VARIANTS] = {
        "set the %s to 20",
        "set the %s to 21.5",
        "%s 21 and a half",
    };
    int set_n = HEATING_ROOMS * VOICE_ID_SET_VARIANTS;
    if (i < set_n) {
        snprintf(buf, len, k_variants[i % VOICE_ID_SET_VARIANTS], s_table[i].room);
    } else if (i < set_n + VOICE_ID_NO_TRV_COUNT) {
        snprintf(buf, len, k_variants[0], s_table[i].room);
    } else {
        int rel = i - (set_n + VOICE_ID_NO_TRV_COUNT);
        snprintf(buf, len, "%s", rel == 0 ? "warm up" : (rel == 1 ? "cool down" : "cancel"));
    }
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
    for (int i = 0; i < VOICE_ID_NO_TRV_COUNT; i++) {
        s_table[n].id = (int16_t)(VOICE_ID_NO_TRV_BASE + i);
        s_table[n].room = k_unheated[i];
        n++;
    }
    s_table[n++] = { VOICE_ID_WARM_UP,   NULL };
    s_table[n++] = { VOICE_ID_COOL_DOWN, NULL };
    s_table[n++] = { VOICE_ID_CANCEL,    NULL };
    s_table_n = n;
}

/**
 * Called from app_audio_init() between its esp_mn_commands_clear() and
 * esp_mn_commands_update(): everything added here becomes the recognised
 * vocabulary. A no-op after the first call, so a failed voice start cannot
 * leave the list half-built.
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
    ESP_LOGI(TAG, "%d of %d English phrases registered", added, s_table_n);
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
            /* Report the value that went on the wire — the card itself only
             * moves when the TRV reports back. */
            int16_t t = heating_clamp_tenth(tenth);
            snprintf(msg, sizeof(msg), "%s \xE2\x86\x92 %d.%d\xC2\xB0" "C",
                     room->cfg.name, (int)(t / 10), (int)((t % 10 + 10) % 10));
            heating_ui_banner(msg);
            heating_ui_poke();
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
    if (!s_registered) return;                 /* a different audio build */
    heating_ui_listening(false);               /* the window closes on a match */

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
    if (phrase_id >= VOICE_ID_NO_TRV_BASE && phrase_id < VOICE_ID_NO_TRV_BASE + VOICE_ID_NO_TRV_COUNT) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s has no smart TRV", k_unheated[phrase_id - VOICE_ID_NO_TRV_BASE]);
        heating_ui_banner(msg);
        return;
    }
    int rel = phrase_id - VOICE_ID_SET_BASE;
    if (rel < 0 || rel >= HEATING_ROOMS * VOICE_ID_SET_VARIANTS) return;
    apply(rel / VOICE_ID_SET_VARIANTS, k_variant_tenth[rel % VOICE_ID_SET_VARIANTS]);
}

/* ── the audio framework's other hooks ───────────────────────────────── */

/* The words this panel knows, logged as the window opens. If what was said is
 * not on this list, nothing will recognise it — that is what the log is for. */
void heating_voice_print_vocabulary(void)
{
    char phrase[VOICE_PHRASE_MAX];
    static char line[1024];
    size_t w = 0;
    w += (size_t)snprintf(line + w, sizeof(line) - w, "voice vocabulary (%d phrases):", s_table_n);
    for (int i = 0; i < s_table_n && w + VOICE_PHRASE_MAX + 4 < sizeof(line); i++) {
        phrase_text(i, phrase, sizeof(phrase));
        w += (size_t)snprintf(line + w, sizeof(line) - w, "%s%s", i ? " |" : "", phrase);
    }
    ESP_LOGI(TAG, "%s", line);
}

void heating_voice_on_event(int listening)
{
    heating_ui_listening(listening != 0);
    if (listening) {
        heating_ui_banner("Listening\xE2\x80\xA6");
    }
}

/* The window closed without a recognised command (timeout or a second press):
 * the spec's 3 s explanation, and the strip back to its idle state. The panel
 * does not say why in the log beyond this — the recogniser's own log states
 * which model and which phrases it holds, so the words can be checked there. */
void heating_voice_not_understood(void)
{
    heating_ui_listening(false);
    heating_ui_banner("Sorry, not understood");
}
