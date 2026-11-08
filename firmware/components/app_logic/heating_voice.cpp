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
 *   "set <room> to <15..25>"                          the same without "the" —
 *                                                     an exact-phrase matcher
 *                                                     hears them as different
 *   "<room> <15..25>"                                 the short form, home range
 *   "set the <room> to <16..23> and a half"           halves, both forms,
 *   "<room> <16..23> and a half"                      said either way:
 *   "set the <room> to <16..23> point five"           "… and a half" or
 *   "<room> <16..23> point five"                      "… point five"
 *   "set all rooms to <5..30>"                        the whole house at once
 *   "set the <roomA> and the <roomB> to <19..21>"     two rooms, either order
 *   "warm up [the <room>]" / "cool down [the <room>]" ±1 °C
 *   "cancel" / "stop"                                 nothing is published
 *   "set the kitchen|bedroom to 20"                   "… has no smart TRV"
 *
 * English only, US English model (mn7_en). The outcome lands on the screen:
 * there are no spoken replies.
 */

#include "heating.h"
#include "app_logic.h"
#include "app_audio.h"
#include "app_config.h"

#include <esp_heap_caps.h>

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
 * under MultiNet's 400-phrase ceiling (396 with four rooms): "set <room> to
 * N" without "the" is its own family — an exact-phrase matcher does not
 * forgive a dropped article — paid for by narrower half and pair ranges. */
#define FULL_MIN   5     /* "set the <room> to N"            */
#define FULL_MAX  30
#define NOTHE_MIN 15     /* "set <room> to N"                */
#define NOTHE_MAX 25
#define BARE_MIN  15     /* "<room> N"                       */
#define BARE_MAX  25
#define HALF_MIN  16     /* "… N and a half" / "… N point five" */
#define HALF_MAX  23
#define PAIR_MIN  19     /* "set the A and the B to N"       */
#define PAIR_MAX  21

/* The rooms that exist in the house but carry no smart TRV — naming one gets
 * "Kitchen has no smart TRV" instead of silence (spec failureModes). */
#define VOICE_NO_TRV_COUNT 2
static const char *const k_unheated[VOICE_NO_TRV_COUNT] = { "Kitchen", "Bedroom" };

typedef enum : uint8_t {
    ACT_SET,          /* room_a -> tenth                         */
    ACT_SET_NOTHE,    /* same, said without "the"                */
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
        case ACT_SET_NOTHE:
            snprintf(buf, len, "set %s to %d", room_name(p->room_a), deg);
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
        for (int d = NOTHE_MIN; d <= NOTHE_MAX; d++) table_push(ACT_SET_NOTHE, r, -1, (int16_t)(d * 10));
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

/* ── the two grammars ─────────────────────────────────────────────────
 *
 * Full sentences carry too many near-neighbours for mn7_en to separate
 * reliably ("twenty two" lost to "sixteen" inside a 396-phrase grammar), so a
 * low-confidence match is never applied: the panel keeps the part the
 * candidates agree on (the room), swaps MultiNet to a NUMBERS-ONLY grammar —
 * 48 phrases instead of 396, where values are acoustically unambiguous — and
 * asks for just the temperature. The swap runs on the recogniser's own task
 * (no concurrent detect), and the full grammar is restored after the answer,
 * a cancel, or a timeout. */

#define NUM_ID_BASE    3000
#define NUM_ID_CANCEL  3999
static bool s_num_grammar = false;        /* which grammar MultiNet holds now */
static int16_t s_num_tenth[64];
static int s_num_n = 0;

static int add_full_grammar(void)
{
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
    return added;
}

static int add_number_grammar(void)
{
    int added = 0;
    char phrase[VOICE_PHRASE_MAX];
    s_num_n = 0;
    for (int d = FULL_MIN; d <= FULL_MAX && s_num_n < 63; d++) {
        snprintf(phrase, sizeof(phrase), "%d", d);
        s_num_tenth[s_num_n] = (int16_t)(d * 10);
        if (esp_mn_commands_add(NUM_ID_BASE + s_num_n, phrase) == ESP_OK) added++;
        s_num_n++;
    }
    for (int d = HALF_MIN; d <= HALF_MAX && s_num_n < 63; d++) {
        snprintf(phrase, sizeof(phrase), "%d and a half", d);
        s_num_tenth[s_num_n] = (int16_t)(d * 10 + 5);
        if (esp_mn_commands_add(NUM_ID_BASE + s_num_n, phrase) == ESP_OK) added++;
        s_num_n++;
        snprintf(phrase, sizeof(phrase), "%d point five", d);
        s_num_tenth[s_num_n] = (int16_t)(d * 10 + 5);
        if (esp_mn_commands_add(NUM_ID_BASE + s_num_n, phrase) == ESP_OK) added++;
        s_num_n++;
    }
    if (esp_mn_commands_add(NUM_ID_CANCEL, "cancel") == ESP_OK) added++;
    return added;
}

/* Swap what MultiNet recognises. Runs only on the recogniser's task (the
 * command/timeout handlers) or at init, so detect never races the rebuild. */
static void set_grammar(bool numbers)
{
    if (s_num_grammar == numbers && s_registered) return;
    esp_mn_commands_clear();
    int added = numbers ? add_number_grammar() : add_full_grammar();
    esp_mn_commands_update();
    s_num_grammar = numbers;
    ESP_LOGI(TAG, "grammar: %s (%d phrases)", numbers ? "numbers only" : "full", added);
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
    int added = add_full_grammar();
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

static bool phrase_is_set(const voice_phrase_t *p)
{
    switch (p->act) {
        case ACT_SET: case ACT_SET_NOTHE: case ACT_SET_HALF: case ACT_SET_P5:
        case ACT_SET_BARE: case ACT_SET_BARE_HALF: case ACT_SET_BARE_P5:
        case ACT_SET_ALL: case ACT_SET_PAIR:
            return true;
        default:
            return false;
    }
}

/* One setpoint to the rooms a SET phrase names — the value may come from the
 * phrase itself or from the number re-decode of the same audio. */
static void apply_scope(const voice_phrase_t *p, int16_t tenth)
{
    if (p->act == ACT_SET_ALL) {
        heating_req_result_t worst = HEATING_REQ_ALREADY_SET;
        for (int r = 0; r < HEATING_ROOMS; r++) {
            heating_req_result_t res = request_one(r, tenth);
            if (res == HEATING_REQ_OK) worst = HEATING_REQ_OK;
            else if (res == HEATING_REQ_OFFLINE && worst != HEATING_REQ_OK) worst = res;
        }
        report_result(worst, "All rooms", tenth);
        return;
    }
    if (p->act == ACT_SET_PAIR) {
        heating_req_result_t ra = request_one(p->room_a, tenth);
        heating_req_result_t rb = request_one(p->room_b, tenth);
        heating_req_result_t res =
            (ra == HEATING_REQ_OK || rb == HEATING_REQ_OK) ? HEATING_REQ_OK :
            (ra == HEATING_REQ_OFFLINE ? ra : rb);
        char what[48];
        snprintf(what, sizeof(what), "%s & %s", room_name(p->room_a), room_name(p->room_b));
        heating_ui_select(p->room_a);
        report_result(res, what, tenth);
        return;
    }
    apply(p->room_a, tenth);
}

static void too_uncertain(void)
{
    if (s_heard[0]) {
        char msg[150];
        snprintf(msg, sizeof(msg), "Heard: \"%s\" (%d%%)\ntoo uncertain - nothing changed",
                 s_heard, s_heard_pct);
        s_heard[0] = '\0';
        heating_ui_banner(msg);
    } else {
        heating_ui_banner("Sorry, not understood");
    }
}

/* ── reinterpreting the SAME utterance ─────────────────────────────────
 *
 * Never applied below the confident bar, and never re-asked either: the
 * window's audio is kept (app_audio records the AFE output), and a doubtful
 * match is settled by decoding that SAME capture again — first the whole
 * utterance gain-normalised against the full grammar, then its tail snippets
 * against a 48-phrase numbers-only grammar, where "twenty two" cannot lose to
 * "sixteen". The sentence pass names the rooms; the number pass names the
 * value; both have to clear their gates or nothing is changed.
 *
 * Gates, measured on this board: a correct full sentence scores 11-16%, so
 * 25% is "already sure", 8% is the floor worth re-examining; the small
 * number grammar is expected to score far higher, so it must clear 15%. */
#define VOICE_SURE_PCT       25
#define VOICE_FLOOR_PCT      8
#define VOICE_NUM_SURE_PCT   15

static void normalize_pcm(int16_t *pcm, size_t n)
{
    int peak = 0;
    for (size_t i = 0; i < n; i++) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
        if (v > peak) peak = v;
    }
    if (peak < 500 || peak >= 20000) return;      /* silence, or already loud */
    int num = 20000, den = peak;
    for (size_t i = 0; i < n; i++) {
        pcm[i] = (int16_t)((int32_t)pcm[i] * num / den);
    }
}

static void reinterpret_window(const voice_phrase_t *live_p, int live_pct)
{
    const int16_t *pcm = NULL;
    size_t n = app_audio_window_audio(&pcm);
    if (pcm == NULL || n < 8000) {                /* under half a second */
        too_uncertain();
        return;
    }

    /* A normalised working copy with half a second of silence at the end, so
     * every pass sees a closed utterance. */
    size_t tail_zeros = 8000;
    int16_t *work = (int16_t *)heap_caps_malloc((n + tail_zeros) * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (work == NULL) {
        too_uncertain();
        return;
    }
    memcpy(work, pcm, n * sizeof(int16_t));
    memset(work + n, 0, tail_zeros * sizeof(int16_t));
    normalize_pcm(work, n);

    /* Pass A: the whole normalised utterance, full grammar — an independent
     * second reading of the same words. */
    int pct_a = 0;
    int id_a = app_audio_decode_buffer(work, n + tail_zeros, &pct_a);
    int ia = id_a - VOICE_ID_SET_BASE;
    const voice_phrase_t *p_a =
        (ia >= 0 && ia < s_table_n && phrase_is_set(&s_table[ia])) ? &s_table[ia] : NULL;
    ESP_LOGI(TAG, "reinterpret: live %d%%, normalised pass id %d at %d%%",
             live_pct, id_a, pct_a);

    /* No tail/number passes: they were tried and retired. They decode the
     * same marginal audio the sentence passes saw (so they confirm nothing —
     * a wrong value once carried a live and a tail pass at once), and the
     * runtime grammar swap they needed rebuilt a 396-phrase FST on the
     * recogniser's task, which aborted the whole panel. Two independent
     * readings of the full sentence are the evidence; agreement is the test. */
    free(work);

    /* The decision: the two SENTENCE readings must independently agree on the
     * action, the rooms and the value. Anything less is shown, not applied —
     * a wrong setpoint is worse than none. A timeout entry (no live match)
     * accepts the normalised pass alone only when it is outright sure. */
    if (live_p != NULL && p_a != NULL &&
        live_p->act == p_a->act && live_p->room_a == p_a->room_a &&
        live_p->room_b == p_a->room_b && live_p->tenth == p_a->tenth &&
        (live_pct >= VOICE_FLOOR_PCT || pct_a >= VOICE_FLOOR_PCT)) {
        ESP_LOGI(TAG, "reinterpret: both readings agree — applying %d.%d",
                 live_p->tenth / 10, (live_p->tenth % 10 + 10) % 10);
        apply_scope(live_p, live_p->tenth);
        return;
    }
    if (live_p == NULL && p_a != NULL && pct_a >= VOICE_SURE_PCT) {
        apply_scope(p_a, p_a->tenth);
        return;
    }
    too_uncertain();
}

/* Called by the audio framework for one recognised phrase id. */
void heating_voice_apply_command(int phrase_id)
{
    if (!s_registered) return;                 /* a different audio build */
    heating_ui_listening(false);               /* the window closes on a match */

    int pct = s_heard_pct;

    /* A number-grammar id arriving HERE means a live detection fired while
     * the reinterpretation grammar was in place (it never should: the window
     * is closed during reinterpretation) — restore and drop it. */
    if (phrase_id >= NUM_ID_BASE) {
        set_grammar(false);
        return;
    }

    int i = phrase_id - VOICE_ID_SET_BASE;
    if (i < 0 || i >= s_table_n) return;
    const voice_phrase_t *p = &s_table[i];

    /* Short phrases are noise magnets (broken audio once matched "stop" at
     * 18% while a CORRECT full sentence scores 11-16%), so they take a high
     * bar and are never reinterpreted. */
    if (!phrase_is_set(p)) {
        if (pct > 0 && pct < 30) {
            too_uncertain();
            return;
        }
        switch (p->act) {
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
        return;
    }

    /* A SET family phrase: sure applies as heard, anything less is settled by
     * re-decoding the same capture — never applied as-is, never re-asked. */
    if (pct >= VOICE_SURE_PCT) {
        apply_scope(p, p->tenth);
        return;
    }
    reinterpret_window(p, pct);
}

/* ── the audio framework's other hooks ───────────────────────────────── */

/* The grammar this panel knows, logged as the window opens. 352 phrases are
 * too many to print one by one, so the log names the families instead. */
void heating_voice_print_vocabulary(void)
{
    ESP_LOGI(TAG, "voice vocabulary: %d phrases — \"set [the] <room> to <%d..%d>\", "
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
    if (!s_registered) return;
    if (s_num_grammar) {
        /* Should not happen (the window is closed while reinterpreting), but
         * never leave the panel listening for bare numbers. */
        set_grammar(false);
    }
    /* The live decoder gave up; the capture may still hold a command — decode
     * the same audio again before giving up (banners come from there). */
    reinterpret_window(NULL, 0);
}
