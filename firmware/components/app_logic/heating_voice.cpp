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
 * MultiNet is a fixed-phrase recogniser (no free slots), so "any room, any
 * value" is an enumerated grid, sized against ESP_MN_MAX_PHRASE_NUM (400):
 *
 *   "set the <room> to <5..30>"                       every room, whole degrees
 *   "<room> <15..25>"                                 the short form, home range
 *   "set the <room> to <15..24> and a half"           halves, both forms,
 *   "<room> <15..24> and a half"                      said either way:
 *   "set the <room> to <15..24> point five"           "… and a half" or
 *   "<room> <15..24> point five"                      "… point five"
 *   "set all rooms to <5..30>"                        the whole house at once
 *   "set the <roomA> and the <roomB> to <18..21>"     two rooms, either order
 *   "warm up [the <room>]" / "cool down [the <room>]" ±1 °C
 *   "cancel" / "stop"                                 nothing is published
 *   "set the kitchen|bedroom to 20"                   "… has no smart TRV"
 *
 * English only, US English model (mn7_en). The outcome lands on the screen:
 * there are no spoken replies.
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

#define VOICE_ID_SET_BASE   1600
#define VOICE_MAX_PHRASES   400   /* = ESP_MN_MAX_PHRASE_NUM */
#define VOICE_PHRASE_MAX    64

/* The whole-degree ranges each phrase family enumerates. Together they stay
 * under MultiNet's 400-phrase ceiling (396 with four rooms): the halves say
 * "and a half" or "point five" in both the full and the bare form, paid for
 * by a narrower two-room conjunction range. */
#define FULL_MIN   5     /* "set the <room> to N"            */
#define FULL_MAX  30
#define BARE_MIN  15     /* "<room> N"                       */
#define BARE_MAX  25
#define HALF_MIN  15     /* "… N and a half" / "… N point five" */
#define HALF_MAX  24
#define PAIR_MIN  18     /* "set the A and the B to N"       */
#define PAIR_MAX  21

/* The rooms that exist in the house but carry no smart TRV — naming one gets
 * "Kitchen has no smart TRV" instead of silence (spec failureModes). */
#define VOICE_NO_TRV_COUNT 2
static const char *const k_unheated[VOICE_NO_TRV_COUNT] = { "Kitchen", "Bedroom" };

typedef enum : uint8_t {
    ACT_SET,          /* room_a -> tenth                         */
    ACT_SET_HALF,     /* same, said "… and a half"               */
    ACT_SET_P5,       /* same, said "… point five"               */
    ACT_SET_BARE,     /* "<room> N"                              */
    ACT_SET_BARE_HALF,
    ACT_SET_BARE_P5,
    ACT_SET_ALL,      /* every room -> tenth                     */
    ACT_SET_PAIR,     /* room_a and room_b -> tenth              */
    ACT_WARM,         /* room_a (or the selected room when -1) +1 °C */
    ACT_COOL,
    ACT_CANCEL,       /* "cancel"                                */
    ACT_STOP,         /* "stop" — same outcome, its own phrase   */
    ACT_NO_TRV,       /* room_a indexes k_unheated               */
} voice_act_t;

typedef struct {
    voice_act_t act;
    int8_t      room_a;
    int8_t      room_b;
    int16_t     tenth;
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

static const char *room_name(int idx)
{
    heating_room_t *r = heating_room(idx);
    return r ? r->cfg.name : "";
}

/* Phrase text for table entry @p i; @p buf must hold VOICE_PHRASE_MAX bytes. */
static void phrase_text(int i, char *buf, size_t len)
{
    const voice_phrase_t *p = &s_table[i];
    int deg = p->tenth / 10;
    switch (p->act) {
        case ACT_SET:
            snprintf(buf, len, "set the %s to %d", room_name(p->room_a), deg);
            break;
        case ACT_SET_HALF:
            snprintf(buf, len, "set the %s to %d and a half", room_name(p->room_a), deg);
            break;
        case ACT_SET_P5:
            snprintf(buf, len, "set the %s to %d point five", room_name(p->room_a), deg);
            break;
        case ACT_SET_BARE:
            snprintf(buf, len, "%s %d", room_name(p->room_a), deg);
            break;
        case ACT_SET_BARE_HALF:
            snprintf(buf, len, "%s %d and a half", room_name(p->room_a), deg);
            break;
        case ACT_SET_BARE_P5:
            snprintf(buf, len, "%s %d point five", room_name(p->room_a), deg);
            break;
        case ACT_SET_ALL:
            snprintf(buf, len, "set all rooms to %d", deg);
            break;
        case ACT_SET_PAIR:
            snprintf(buf, len, "set the %s and the %s to %d",
                     room_name(p->room_a), room_name(p->room_b), deg);
            break;
        case ACT_WARM:
            if (p->room_a < 0) snprintf(buf, len, "warm up");
            else               snprintf(buf, len, "warm up the %s", room_name(p->room_a));
            break;
        case ACT_COOL:
            if (p->room_a < 0) snprintf(buf, len, "cool down");
            else               snprintf(buf, len, "cool down the %s", room_name(p->room_a));
            break;
        case ACT_CANCEL:
            snprintf(buf, len, "cancel");
            break;
        case ACT_STOP:
            snprintf(buf, len, "stop");
            break;
        case ACT_NO_TRV:
            snprintf(buf, len, "set the %s to 20", k_unheated[p->room_a]);
            break;
        default:
            buf[0] = '\0';
            break;
    }
}

static void table_push(voice_act_t act, int room_a, int room_b, int16_t tenth)
{
    if (s_table_n >= VOICE_MAX_PHRASES) return;
    s_table[s_table_n].act    = act;
    s_table[s_table_n].room_a = (int8_t)room_a;
    s_table[s_table_n].room_b = (int8_t)room_b;
    s_table[s_table_n].tenth  = tenth;
    s_table_n++;
}

static void build_table(void)
{
    s_table_n = 0;
    for (int r = 0; r < HEATING_ROOMS; r++) {
        for (int d = FULL_MIN; d <= FULL_MAX; d++) table_push(ACT_SET, r, -1, (int16_t)(d * 10));
        for (int d = BARE_MIN; d <= BARE_MAX; d++) table_push(ACT_SET_BARE, r, -1, (int16_t)(d * 10));
        for (int d = HALF_MIN; d <= HALF_MAX; d++) {
            table_push(ACT_SET_HALF,      r, -1, (int16_t)(d * 10 + 5));
            table_push(ACT_SET_BARE_HALF, r, -1, (int16_t)(d * 10 + 5));
            table_push(ACT_SET_P5,        r, -1, (int16_t)(d * 10 + 5));
            table_push(ACT_SET_BARE_P5,   r, -1, (int16_t)(d * 10 + 5));
        }
    }
    for (int d = FULL_MIN; d <= FULL_MAX; d++) table_push(ACT_SET_ALL, -1, -1, (int16_t)(d * 10));
    for (int a = 0; a < HEATING_ROOMS; a++) {
        for (int b = 0; b < HEATING_ROOMS; b++) {
            if (a == b) continue;
            for (int d = PAIR_MIN; d <= PAIR_MAX; d++) {
                table_push(ACT_SET_PAIR, a, b, (int16_t)(d * 10));
            }
        }
    }
    table_push(ACT_WARM, -1, -1, 0);
    table_push(ACT_COOL, -1, -1, 0);
    for (int r = 0; r < HEATING_ROOMS; r++) {
        table_push(ACT_WARM, r, -1, 0);
        table_push(ACT_COOL, r, -1, 0);
    }
    table_push(ACT_CANCEL, -1, -1, 0);
    table_push(ACT_STOP,   -1, -1, 0);
    for (int i = 0; i < VOICE_NO_TRV_COUNT; i++) table_push(ACT_NO_TRV, i, -1, 0);
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
        if (esp_mn_commands_add(VOICE_ID_SET_BASE + i, phrase) == ESP_OK) {
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

static heating_req_result_t request_one(int idx, int16_t tenth)
{
    return heating_request_target(idx, tenth);
}

/* What the recogniser matched, quoted on the outcome banner so the wall says
 * what it HEARD, not only what it did. Set per command, consumed by the next
 * banner. Both writers run on the audio detect task, in order. */
static char s_heard[72] = {0};
static int  s_heard_pct = 0;

void heating_voice_heard(const char *text, int percent)
{
    snprintf(s_heard, sizeof(s_heard), "%s", text ? text : "");
    s_heard_pct = percent;
}

/* The outcome, under the heard phrase when there is one: two lines on the
 * banner, '"office 21" (97%)' over 'Office -> 21.0C'. */
static void banner_outcome(const char *outcome)
{
    char msg[150];
    if (s_heard[0]) {
        snprintf(msg, sizeof(msg), "\"%s\" (%d%%)\n%s", s_heard, s_heard_pct, outcome);
        s_heard[0] = '\0';
    } else {
        snprintf(msg, sizeof(msg), "%s", outcome);
    }
    heating_ui_banner(msg);
    heating_ui_poke();
}

static void banner_set(const char *what, int16_t tenth)
{
    char msg[64];
    /* ASCII only: the LCD's built-in fonts carry no arrows or em dashes.
     * Report the value that went on the wire — the card itself only moves
     * when the TRV reports back. */
    int16_t t = heating_clamp_tenth(tenth);
    snprintf(msg, sizeof(msg), "%s -> %d.%d\xC2\xB0" "C", what, (int)(t / 10), (int)((t % 10 + 10) % 10));
    banner_outcome(msg);
}

static void report_result(heating_req_result_t res, const char *what, int16_t tenth)
{
    switch (res) {
        case HEATING_REQ_OK:
            banner_set(what, tenth);
            break;
        case HEATING_REQ_ALREADY_SET:
            banner_outcome("Already set");
            break;
        case HEATING_REQ_OFFLINE:
            banner_outcome("Offline - controls disabled");
            heating_ui_rejected();
            break;
        default:
            break;
    }
}

static void apply(int idx, int16_t tenth)
{
    heating_room_t *room = heating_room(idx);
    if (room == NULL) return;
    heating_ui_select(idx);
    report_result(request_one(idx, tenth), room->cfg.name, tenth);
}

/* Called by the audio framework for one recognised phrase id. */
void heating_voice_apply_command(int phrase_id)
{
    if (!s_registered) return;                 /* a different audio build */
    heating_ui_listening(false);               /* the window closes on a match */

    int i = phrase_id - VOICE_ID_SET_BASE;
    if (i < 0 || i >= s_table_n) return;
    const voice_phrase_t *p = &s_table[i];

    switch (p->act) {
        case ACT_SET:
        case ACT_SET_HALF:
        case ACT_SET_P5:
        case ACT_SET_BARE:
        case ACT_SET_BARE_HALF:
        case ACT_SET_BARE_P5:
            apply(p->room_a, p->tenth);
            break;
        case ACT_SET_ALL: {
            heating_req_result_t worst = HEATING_REQ_ALREADY_SET;
            for (int r = 0; r < HEATING_ROOMS; r++) {
                heating_req_result_t res = request_one(r, p->tenth);
                if (res == HEATING_REQ_OK) worst = HEATING_REQ_OK;
                else if (res == HEATING_REQ_OFFLINE && worst != HEATING_REQ_OK) worst = res;
            }
            report_result(worst, "All rooms", p->tenth);
            break;
        }
        case ACT_SET_PAIR: {
            heating_req_result_t ra = request_one(p->room_a, p->tenth);
            heating_req_result_t rb = request_one(p->room_b, p->tenth);
            heating_req_result_t res =
                (ra == HEATING_REQ_OK || rb == HEATING_REQ_OK) ? HEATING_REQ_OK :
                (ra == HEATING_REQ_OFFLINE ? ra : rb);
            char what[48];
            snprintf(what, sizeof(what), "%s & %s", room_name(p->room_a), room_name(p->room_b));
            heating_ui_select(p->room_a);
            report_result(res, what, p->tenth);
            break;
        }
        case ACT_WARM:
        case ACT_COOL: {
            int idx = (p->room_a >= 0) ? p->room_a : s_sel_room;
            heating_room_t *room = heating_room(idx);
            if (room == NULL) return;
            int16_t step = (p->act == ACT_WARM) ? 10 : -10;
            apply(idx, (int16_t)(heating_room_target_ui(room) + step));
            break;
        }
        case ACT_CANCEL:
        case ACT_STOP:
            banner_outcome("Cancelled");
            break;
        case ACT_NO_TRV: {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s has no smart TRV", k_unheated[p->room_a]);
            banner_outcome(msg);
            break;
        }
        default:
            break;
    }
}

/* ── the audio framework's other hooks ───────────────────────────────── */

/* The grammar this panel knows, logged as the window opens. 352 phrases are
 * too many to print one by one, so the log names the families instead. */
void heating_voice_print_vocabulary(void)
{
    ESP_LOGI(TAG, "voice vocabulary: %d phrases — \"set the <room> to <%d..%d>\", "
                  "\"<room> <%d..%d>\", \"... <%d..%d> and a half | point five\", "
                  "\"set all rooms to <N>\", \"set the <A> and the <B> to <%d..%d>\", "
                  "\"warm up / cool down [the <room>]\", \"cancel\", \"stop\"",
             s_table_n, FULL_MIN, FULL_MAX, BARE_MIN, BARE_MAX, HALF_MIN, HALF_MAX,
             PAIR_MIN, PAIR_MAX);
}

void heating_voice_on_event(int listening)
{
    heating_ui_listening(listening != 0);
    if (listening) {
        heating_ui_banner("Listening...");
    }
}

/* The window closed without a recognised command (timeout or a second press):
 * the spec's 3 s explanation, and the strip back to its idle state. The panel
 * does not say why in the log beyond this — the recogniser's own log states
 * which model and which phrases it holds, so the words can be checked there. */
void heating_voice_not_understood(void)
{
    heating_ui_listening(false);
    s_heard[0] = '\0';
    heating_ui_banner("Sorry, not understood\ntry: set the office to 21");
}
