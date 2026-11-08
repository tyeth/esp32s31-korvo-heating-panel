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
 *   "set the <room> to <15..25>"                      the comfort band only —
 *   "set <room> to <15..25>"                          frost extremes are not
 *   "set <room> <15..25>"                             voice-reachable; each
 *   "<room> <15..25>"                                 wording is its own phrase
 *                                                     (exact-phrase matcher)
 *   "set the <room> to <16..23> and a half"           halves, either wording:
 *   "<room> <16..23> and a half"                      "… and a half" or
 *   "set the <room> to <16..23> point five"           "… point five"
 *   "<room> <16..23> point five"
 *   "set all rooms to <15..25>"                       the whole house at once
 *   "set the <roomA> and the <roomB> to <19..22>"     two rooms, either order
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
#include <esp_system.h>

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
/* Voice commands cover the MAIN HEATING setpoint band only — 15..25 °C in
 * every phrase form, rooms and all-rooms alike. Frost-protection and other
 * extremes are deliberately not reachable by voice: the settings page and the
 * touch panel own those. */
#define FULL_MIN  15
#define FULL_MAX  25
#define NOTHE_MIN FULL_MIN
#define NOTHE_MAX FULL_MAX
#define BARE_MIN  FULL_MIN
#define BARE_MAX  FULL_MAX
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
    ACT_SET_NOTO,     /* same, said without "the" or "to"        */
    ACT_SET_HALF,     /* same, said "… and a half"               */
    ACT_SET_P5,       /* same, said "… point five"               */
    ACT_SET_BARE,     /* "<room> N"                              */
    ACT_SET_BARE_HALF,
    ACT_SET_BARE_P5,
    ACT_NUM,          /* a bare "N" -> the selected room; also the value
                       * evidence the tail passes read, with no grammar swap */
    ACT_NUM_HALF,
    ACT_NUM_P5,
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
        case ACT_SET_NOTO:
            snprintf(buf, len, "set %s %d", room_name(p->room_a), deg);
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
        case ACT_NUM:
            snprintf(buf, len, "%d", deg);
            break;
        case ACT_NUM_HALF:
            snprintf(buf, len, "%d and a half", deg);
            break;
        case ACT_NUM_P5:
            snprintf(buf, len, "%d point five", deg);
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
        for (int d = NOTHE_MIN; d <= NOTHE_MAX; d++) {
            table_push(ACT_SET_NOTHE, r, -1, (int16_t)(d * 10));
            table_push(ACT_SET_NOTO,  r, -1, (int16_t)(d * 10));
        }
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
    /* Bare values, resident in the ONE grammar: "23" spoken live sets the
     * selected room, and the tail passes read these same phrases as value
     * evidence — no runtime grammar swap, no 15-second FST rebuild, none of
     * the idle starvation that rebooted the panel mid-conversation. */
    for (int d = FULL_MIN; d <= FULL_MAX; d++) table_push(ACT_NUM, -1, -1, (int16_t)(d * 10));
    for (int d = HALF_MIN; d <= HALF_MAX; d++) {
        table_push(ACT_NUM_HALF, -1, -1, (int16_t)(d * 10 + 5));
        table_push(ACT_NUM_P5,   -1, -1, (int16_t)(d * 10 + 5));
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

/* ── one grammar, built once ──────────────────────────────────────────
 *
 * There used to be a second, numbers-only grammar swapped in at runtime for
 * the tail passes. Each swap rebuilt an FST on the recogniser's task — the
 * full one took ~15 s of solid CPU, starved the idle task, and the task
 * watchdog rebooted the panel when a second command arrived mid-rebuild. The
 * bare-number phrases (ACT_NUM*) live in the ONE grammar now: the tail passes
 * read them as value evidence, and spoken live, a bare "23" sets the room
 * selected on screen. Nothing is ever rebuilt after boot. */

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
        case ACT_SET: case ACT_SET_NOTHE: case ACT_SET_NOTO:
        case ACT_SET_HALF: case ACT_SET_P5:
        case ACT_SET_BARE: case ACT_SET_BARE_HALF: case ACT_SET_BARE_P5:
        case ACT_NUM: case ACT_NUM_HALF: case ACT_NUM_P5:
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
    /* A bare number names no room: it goes to the room selected on screen. */
    apply(p->room_a >= 0 ? p->room_a : s_sel_room, tenth);
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

/* True when @p ext is what @p base sounds like with MORE WORDS after it — the
 * FST's early-exit shapes, in tenths of a degree:
 *   twenty (200)        -> twenty one..twenty nine (210..290)
 *   X                   -> X and a half / X point five (X+5)
 *   six..nine (60..90)  -> sixteen..nineteen (160..190)
 *   (five/fifteen and four/fourteen do not share a spoken prefix)
 */
static bool value_extends(int16_t base, int16_t ext)
{
    if (ext == base) return false;
    if (ext == base + 5) return true;                       /* "... and a half" */
    if (base % 10 == 0 && ext % 10 == 0) {
        int b = base / 10, e = ext / 10;
        if (b == 20 && e > 20 && e < 30) return true;       /* twenty -> twenty X */
        if (b >= 6 && b <= 9 && e == b + 10) return true;   /* six -> sixteen ... */
    }
    /* twenty -> twenty X and a half: two words further along the same path. */
    if (base == 200 && ext > 200 && ext < 300) return true;
    return false;
}

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

    /* The offline passes own the CPU for a few seconds: stop feeding the AFE
     * meanwhile (the mic keeps draining), resumed on every exit below. */
    app_audio_pause_feed(true);

    /* Pass A: the whole normalised utterance, full grammar — an independent
     * second reading of the same words. */
    int pct_a = 0;
    int id_a = app_audio_decode_buffer(work, n + tail_zeros, &pct_a);
    int ia = id_a - VOICE_ID_SET_BASE;
    const voice_phrase_t *p_a =
        (ia >= 0 && ia < s_table_n && phrase_is_set(&s_table[ia])) ? &s_table[ia] : NULL;
    ESP_LOGI(TAG, "reinterpret: live %d%%, normalised pass id %d at %d%%",
             live_pct, id_a, pct_a);

    /* The decision: the two SENTENCE readings must independently agree on the
     * action, the rooms and the value. Anything less is shown, not applied —
     * a wrong setpoint is worse than none. A timeout entry (no live match)
     * accepts the normalised pass alone only when it is outright sure. */
    bool agree = (live_p != NULL && p_a != NULL &&
                  live_p->act == p_a->act && live_p->room_a == p_a->room_a &&
                  live_p->room_b == p_a->room_b && live_p->tenth == p_a->tenth &&
                  (live_pct >= VOICE_FLOOR_PCT || pct_a >= VOICE_FLOOR_PCT));
    if (!agree) {
        free(work);
        app_audio_pause_feed(false);
        if (live_p == NULL && p_a != NULL && pct_a >= VOICE_SURE_PCT) {
            apply_scope(p_a, p_a->tenth);
            return;
        }
        too_uncertain();
        return;
    }

    /* Agreed on V — but the FST can exit early on a spoken prefix, and both
     * readings truncate TOGETHER ("set bathroom to twenty four" has applied
     * as "... to twenty"). So the value is checked once more against the
     * SAME audio's tail with the numbers-only grammar (48 phrases, swapped at
     * runtime — the detect task's stack was grown for the FST rebuild):
     *   - tail silent or agrees with V        -> apply V
     *   - tail hears an EXTENSION of V        -> apply the extension
     *     (twenty -> twenty one..nine, X -> X and a half, six -> sixteen)
     *   - tail hears something else entirely  -> nothing is applied
     */
    int16_t v_s = live_p->tenth;
    static const size_t k_tails_s10[] = { 15, 25, 40 };   /* seconds x10 */
    int best_tenth = -1, best_npct = 0;
    for (size_t t = 0; t < sizeof(k_tails_s10) / sizeof(k_tails_s10[0]); t++) {
        size_t tn = k_tails_s10[t] * 1600;
        if (tn > n) tn = n;
        int npct = 0;
        int nid = app_audio_decode_buffer(work + (n - tn), tn + tail_zeros, &npct);
        /* Only a BARE-NUMBER phrase counts as tail evidence: a tail that
         * re-matches a whole sentence is just the same reading again. */
        int ni = nid - VOICE_ID_SET_BASE;
        const voice_phrase_t *np = (ni >= 0 && ni < s_table_n) ? &s_table[ni] : NULL;
        bool is_num = np != NULL && (np->act == ACT_NUM || np->act == ACT_NUM_HALF ||
                                     np->act == ACT_NUM_P5);
        ESP_LOGI(TAG, "reinterpret: tail %.1fs -> id %d at %d%%%s",
                 k_tails_s10[t] / 10.0, nid, npct, is_num ? " (number)" : "");
        if (is_num && npct > best_npct) {
            best_npct = npct;
            best_tenth = np->tenth;
        }
        if (tn >= n) break;
    }
    free(work);

    /* Tail evidence by strength, measured on this board: a mid-phrase
     * fragment ("...room to twenty four" against a numbers grammar) scores
     * garbage at up to ~20%, and once vetoed a CORRECT agreed value. So a
     * contradiction must beat that ceiling to block anything; an extension is
     * structurally tied to the agreed value (noise rarely lands on one), so
     * it corrects from the ordinary floor; anything weaker is ignored. */
    #define VOICE_TAIL_VETO_PCT 22
    if (best_tenth >= 0 && best_npct >= VOICE_FLOOR_PCT &&
        best_tenth != v_s && value_extends(v_s, best_tenth)) {
        ESP_LOGI(TAG, "reinterpret: sentence said %d.%d but the tail heard its "
                      "extension %d.%d (%d%%) — the FST truncated; applying the tail",
                 v_s / 10, (v_s % 10 + 10) % 10,
                 best_tenth / 10, (best_tenth % 10 + 10) % 10, best_npct);
        apply_scope(live_p, best_tenth);
    } else if (best_tenth >= 0 && best_npct >= VOICE_TAIL_VETO_PCT &&
               best_tenth != v_s && !value_extends(v_s, best_tenth)) {
        ESP_LOGI(TAG, "reinterpret: tail contradicts the sentence (%d.%d vs %d.%d "
                      "at %d%%) — nothing applied",
                 best_tenth / 10, (best_tenth % 10 + 10) % 10,
                 v_s / 10, (v_s % 10 + 10) % 10, best_npct);
        too_uncertain();
    } else {
        ESP_LOGI(TAG, "reinterpret: both readings agree — applying %d.%d%s",
                 v_s / 10, (v_s % 10 + 10) % 10,
                 best_tenth == v_s ? " (tail confirms)" : "");
        apply_scope(live_p, v_s);
    }
    app_audio_pause_feed(false);
}

/* Called by the audio framework for one recognised phrase id. */
void heating_voice_apply_command(int phrase_id)
{
    if (!s_registered) return;                 /* a different audio build */
    heating_ui_listening(false);               /* the window closes on a match */

    int pct = s_heard_pct;

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
    /* The live decoder gave up; the capture may still hold a command — decode
     * the same audio again before giving up (banners come from there). */
    reinterpret_window(NULL, 0);
}
