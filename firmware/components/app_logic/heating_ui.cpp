/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * The room dashboard on the board's own RGB panel.
 *
 * Four cards in a 2×2 grid — name, current temperature (air sensor, large),
 * the TRV-reported target and a heat/off tag — plus a footer with the link
 * state and the clock. Cards older than 10 minutes are greyed and stamped
 * with the time they were last seen. Nothing here is a wizard or a splash:
 * the cards are on screen from the first frame, using the targets saved in
 * flash, and fill in as the TRVs report.
 *
 * Touch is served by the board's GT1151 through a private LVGL input device:
 * one tap selects a card, a second tap on −/+ changes that room's target.
 */

#include "heating.h"
#include "app_display.h"
#include "app_config.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lvgl.h>
#include "esp_lvgl_port.h"

static const char *TAG = "heating_ui";

#define CARD_W        360
#define CARD_H        170
#define GRID_X        16
#define GRID_Y        12
#define GRID_GAP_X    24
#define GRID_GAP_Y    16
#define FOOTER_H      44

#define BANNER_MS     3000
#define LISTEN_MS     10000
#define DIM_AFTER_MS  (2 * 60 * 1000)

typedef struct {
    lv_obj_t *card;
    lv_obj_t *name;
    lv_obj_t *current;
    lv_obj_t *stamp;
    lv_obj_t *target;
    lv_obj_t *mode;
    lv_obj_t *minus;
    lv_obj_t *plus;
} card_widgets_t;

static lv_obj_t   *s_screen = NULL;
static card_widgets_t s_cards[HEATING_ROOMS];
static lv_obj_t   *s_footer = NULL;
static lv_obj_t   *s_banner = NULL;
static lv_obj_t   *s_listen_box = NULL;
static lv_obj_t   *s_listen_bar = NULL;

/* Selected room, -1 before the first tap. */
static int s_sel = -1;
static bool s_online_ui = false;
static volatile bool s_ui_ready = false;
static volatile bool s_refresh = true;
static volatile bool s_banner_dirty = false;
static volatile bool s_listening = false;
static volatile int  s_listen_s = 0;

static char         s_banner_text[64] = {0};
static int64_t      s_banner_until = 0;
static bool         s_rejecting = false;
static int64_t      s_reject_until = 0;
static int64_t      s_last_activity = 0;
static bool         s_dimmed = false;

/* Touch: the display framework's pointer device and the last press point. */
static lv_indev_t *s_touch_indev = NULL;
static int s_touch_x = -1;
static int s_touch_y = -1;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch_activity(void)
{
    s_last_activity = now_ms();
}

/* ── the model's hooks ───────────────────────────────────────────────── */

/** Activity outside a press: a reading, a link change, a heard command, a
 *  setpoint. Anything the dashboard mirrors waking up counts. */
void heating_ui_poke(void)
{
    touch_activity();
    s_refresh = true;
}

void heating_ui_invalidate(void)
{
    s_refresh = true;
}

void heating_ui_select(int idx)
{
    if (idx >= 0 && idx < HEATING_ROOMS) {
        s_sel = idx;
        heating_voice_set_selected_room(idx);
        s_refresh = true;
    }
}

void heating_ui_banner(const char *text)
{
    touch_activity();          /* a banner is action — the screen stays bright */
    snprintf(s_banner_text, sizeof(s_banner_text), "%s", text ? text : "");
    s_banner_until = now_ms() + BANNER_MS;
    s_banner_dirty = true;
}

void heating_ui_rejected_ui(void)
{
    s_rejecting = true;
    s_reject_until = now_ms() + 1000;
    s_refresh = true;
}

void heating_ui_listening_ui(bool listening)
{
    s_listening = listening;
    if (listening) {
        s_listen_s = LISTEN_MS / 1000;
    }
    s_refresh = true;
}

void heating_voice_seconds_left(int seconds)
{
    s_listen_s = seconds;
    s_refresh = true;
}

/* ── touch input ─────────────────────────────────────────────────────── */

static void touch_hit(int x, int y);
static void on_touch_press(lv_event_t *e);
static void on_touch_release(lv_event_t *e);

/**
 * Select-or-adjust on one tap — the spec's single active room: a tap inside a
 * card selects it (and only its −/+ are live), a tap on the selected card's
 * left or right third changes its target by 0.5 °C and publishes it.
 */
static void touch_hit(int x, int y)
{
    touch_activity();
    for (int i = 0; i < HEATING_ROOMS; i++) {
        int cx = GRID_X + (i % 2) * (CARD_W + GRID_GAP_X);
        int cy = GRID_Y + (i / 2) * (CARD_H + GRID_GAP_Y);
        if (x < cx || x >= cx + CARD_W || y < cy || y >= cy + CARD_H) continue;

        heating_room_t *room = heating_room(i);
        if (i != s_sel) {
            heating_ui_select(i);
            return;
        }
        int local = x - cx;
        int16_t step = 0;
        if (local < CARD_W / 3) step = -HEATING_STEP_TENTH;
        else if (local >= CARD_W * 2 / 3) step = HEATING_STEP_TENTH;
        else return;                               /* middle: keep it selected */

        int16_t want = (int16_t)(heating_room_target_ui(room) + step);
        switch (heating_request_target(i, want)) {
            case HEATING_REQ_OK:
                break;
            case HEATING_REQ_ALREADY_SET:
                heating_ui_banner("Already set");
                break;
            case HEATING_REQ_OFFLINE:
                heating_ui_banner("Offline - controls disabled");
                heating_ui_rejected();
                break;
            default:
                break;
        }
        s_refresh = true;
        return;
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);
static void popup_close(void);
static void heating_ui_settings_popup(void);

/* ── the panel's settings: one overlay, never a second screen ──────────
 *
 * The settings form is the web page. The panel has nothing to edit, but the
 * gear on the touch panel is what people press: it opens a small overlay OVER
 * the cards saying where the page is, and the cards stay exactly where they
 * were. Nothing here creates, loads or clears an LVGL screen — that is what a
 * blank panel would look like. */
static lv_obj_t *s_settings_popup = NULL;

static void popup_close(void)
{
    if (s_settings_popup != NULL) {
        lv_obj_delete(s_settings_popup);
        s_settings_popup = NULL;
    }
}

static void on_popup_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        touch_activity();
        popup_close();
    }
}

void heating_ui_settings_enter(const char *source)
{
    /* The form lives in the browser, so the panel has nothing to leave and no
     * reason to stop drawing: show where it is and keep the cards up. */
    (void)source;
    touch_activity();
    heating_ui_banner("Settings: korvo-heating.local/settings");
    heating_ui_settings_popup();
}

void heating_ui_settings_popup(void)
{
    touch_activity();
    if (!s_ui_ready || s_screen == NULL) return;
    /* Created on the LVGL task (it holds the lock); the order is the port's. */
    if (!lvgl_port_lock(1000)) return;
    popup_close();

    s_settings_popup = lv_obj_create(s_screen);
    lv_obj_set_size(s_settings_popup, CARD_W + 120, 160);
    lv_obj_center(s_settings_popup);
    lv_obj_set_style_bg_color(s_settings_popup, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(s_settings_popup, lv_color_hex(0xE0A06A), 0);
    lv_obj_set_style_border_width(s_settings_popup, 2, 0);
    lv_obj_set_style_radius(s_settings_popup, 12, 0);
    lv_obj_set_style_pad_all(s_settings_popup, 14, 0);
    lv_obj_remove_flag(s_settings_popup, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(s_settings_popup, &lv_font_montserrat_16, lv_color_hex(0xE0A06A));
    lv_label_set_text(title, "Settings are on the web page");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *body = make_label(s_settings_popup, &lv_font_montserrat_14, lv_color_hex(0xCCCCCC));
    lv_label_set_text(body, "Wi-Fi, the MQTT broker and the room topics:\n"
                            "http://korvo-heating.local/settings\n"
                            "(the IP is in the boot log)");
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *hint = make_label(s_settings_popup, &lv_font_montserrat_10, lv_color_hex(0x888888));
    lv_label_set_text(hint, "Tap anywhere to close");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);

    lv_obj_add_flag(s_settings_popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_settings_popup, on_popup_event, LV_EVENT_PRESSED, NULL);

    lvgl_port_unlock();
}

/* ── painting ─────────────────────────────────────────────────────────── */

static void style_panel(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0x333333), 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_label_set_text(lbl, "");
    return lbl;
}

static void build_screen(void)
{
    s_screen = lv_screen_active();
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0F0F0F), 0);

    for (int i = 0; i < HEATING_ROOMS; i++) {
        card_widgets_t *w = &s_cards[i];
        int cx = GRID_X + (i % 2) * (CARD_W + GRID_GAP_X);
        int cy = GRID_Y + (i / 2) * (CARD_H + GRID_GAP_Y);

        w->card = lv_obj_create(s_screen);
        lv_obj_set_pos(w->card, cx, cy);
        lv_obj_set_size(w->card, CARD_W, CARD_H);
        lv_obj_remove_flag(w->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(w->card, 12, 0);
        style_panel(w->card);

        w->name = make_label(w->card, &lv_font_montserrat_20, lv_color_hex(0xE0A06A));
        lv_obj_set_pos(w->name, 0, 0);

        w->mode = make_label(w->card, &lv_font_montserrat_14, lv_color_hex(0x7FD4A3));
        lv_obj_align(w->mode, LV_ALIGN_TOP_RIGHT, 0, 4);

        w->current = make_label(w->card, &lv_font_montserrat_48, lv_color_hex(0xF0F0F0));
        lv_obj_set_pos(w->current, 0, 34);

        w->stamp = make_label(w->card, &lv_font_montserrat_12, lv_color_hex(0x888888));
        lv_obj_align(w->stamp, LV_ALIGN_BOTTOM_LEFT, 0, -28);

        /* Target in the middle, − and + in the corners — matching touch_hit's
         * thirds (left third −, right third +). The glyphs are LVGL's own
         * symbol set: the built-in Montserrat fonts carry only ASCII plus the
         * FontAwesome symbols, so U+2212 MINUS SIGN rendered as a tofu box. */
        w->target = make_label(w->card, &lv_font_montserrat_24, lv_color_hex(0x9AD0F5));
        lv_obj_align(w->target, LV_ALIGN_BOTTOM_MID, 0, 0);

        w->minus = make_label(w->card, &lv_font_montserrat_24, lv_color_hex(0x888888));
        lv_label_set_text(w->minus, LV_SYMBOL_MINUS);
        lv_obj_align(w->minus, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        w->plus = make_label(w->card, &lv_font_montserrat_24, lv_color_hex(0x888888));
        lv_label_set_text(w->plus, LV_SYMBOL_PLUS);
        lv_obj_align(w->plus, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

        /* LVGL events do not bubble without LV_OBJ_FLAG_EVENT_BUBBLE, and the
         * cards (plain lv_obj's) are clickable, so they swallowed every tap:
         * the press/release handlers registered on the screen below never
         * fired for a touch that landed on a card — which is to say, for
         * every touch that was supposed to do anything. The same handlers go
         * on each card; coordinates are absolute either way. */
        lv_obj_add_event_cb(w->card, on_touch_press, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(w->card, on_touch_release, LV_EVENT_RELEASED, NULL);
    }

    s_footer = make_label(s_screen, &lv_font_montserrat_14, lv_color_hex(0xAAAAAA));
    lv_obj_align(s_footer, LV_ALIGN_BOTTOM_MID, 0, -8);

    s_banner = make_label(s_screen, &lv_font_montserrat_20, lv_color_hex(0x000000));
    lv_obj_set_style_bg_color(s_banner, lv_color_hex(0xE0A06A), 0);
    lv_obj_set_style_pad_hor(s_banner, 16, 0);
    lv_obj_set_style_pad_ver(s_banner, 8, 0);
    lv_obj_set_style_radius(s_banner, 8, 0);
    lv_obj_align(s_banner, LV_ALIGN_TOP_MID, 0, GRID_Y * 2 + CARD_H * 2 + GRID_GAP_Y - 30);
    lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);

    /* The 10 s listening bar: full at the wake word, shrinking to nothing. */
    s_listen_box = lv_obj_create(s_screen);
    lv_obj_set_size(s_listen_box, CARD_W * 2 + GRID_GAP_X, 22);
    lv_obj_align(s_listen_box, LV_ALIGN_TOP_MID, 0, GRID_Y * 2 + CARD_H * 2 + GRID_GAP_Y + 8);
    lv_obj_set_style_bg_color(s_listen_box, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(s_listen_box, 6, 0);
    lv_obj_set_style_border_width(s_listen_box, 0, 0);
    lv_obj_remove_flag(s_listen_box, LV_OBJ_FLAG_SCROLLABLE);

    s_listen_bar = lv_obj_create(s_listen_box);
    lv_obj_set_height(s_listen_bar, 22);
    lv_obj_set_style_bg_color(s_listen_bar, lv_color_hex(0xE0A06A), 0);
    lv_obj_set_style_radius(s_listen_bar, 6, 0);
    lv_obj_set_style_border_width(s_listen_bar, 0, 0);
    lv_obj_set_pos(s_listen_bar, 0, 0);
    lv_obj_add_flag(s_listen_box, LV_OBJ_FLAG_HIDDEN);
}

static void paint_card(int i)
{
    heating_room_t *room = heating_room(i);
    card_widgets_t *w = &s_cards[i];

    lv_label_set_text(w->name, room->cfg.name);

    int16_t tenth = 0;
    bool stale = true;
    int64_t seen = 0;
    char buf[48];
    if (heating_room_current(room, &tenth, &stale, &seen)) {
        snprintf(buf, sizeof(buf), "%d.%d\xC2\xB0", (int)(tenth / 10), (int)((tenth % 10 + 10) % 10));
        lv_label_set_text(w->current, buf);
        lv_obj_set_style_text_color(w->current, stale ? lv_color_hex(0x777777) : lv_color_hex(0xF0F0F0), 0);
        char when[16];
        if (stale && heating_format_time(seen, when, sizeof(when))) {
            snprintf(buf, sizeof(buf), "seen %s", when);
            lv_label_set_text(w->stamp, buf);
        } else {
            lv_label_set_text(w->stamp, "");
        }
    } else {
        lv_label_set_text(w->current, "--.-");
        lv_obj_set_style_text_color(w->current, lv_color_hex(0x777777), 0);
        lv_label_set_text(w->stamp, "no reading");
    }

    int16_t target = heating_room_target_ui(room);
    snprintf(buf, sizeof(buf), "%d.%d\xC2\xB0", (int)(target / 10), (int)((target % 10 + 10) % 10));
    lv_label_set_text(w->target, buf);
    lv_obj_set_style_text_color(w->target, s_online_ui ? lv_color_hex(0x9AD0F5) : lv_color_hex(0x666666), 0);

    bool heating = (room->mode == HEATING_HEATING);
    lv_label_set_text(w->mode, heating ? "heat" : "off");
    lv_obj_set_style_text_color(w->mode, heating ? lv_color_hex(0x7FD4A3) : lv_color_hex(0x888888), 0);

    bool selected = (i == s_sel);
    lv_obj_set_style_border_color(w->card, selected ? lv_color_hex(0xE0A06A) : lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(w->card, selected ? 3 : 1, 0);
    /* Only the selected room's −/+ are live (and only while online). */
    lv_color_t ctrl = (selected && s_online_ui) ? lv_color_hex(0x9AD0F5) : lv_color_hex(0x444444);
    lv_obj_set_style_text_color(w->minus, ctrl, 0);
    lv_obj_set_style_text_color(w->plus, ctrl, 0);
}

static void paint_footer(void)
{
    char buf[80];
    char clock[16] = "--:--";
    time_t t = time(NULL);
    if (t > 1600000000) {
        struct tm lt = {};
        localtime_r(&t, &lt);
        snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
    }
    /* ASCII only on the LCD: the built-in fonts have no em dash. */
    if (!heating_online()) {
        snprintf(buf, sizeof(buf), "Offline - controls disabled      %s", clock);
    } else {
        int live = 0;
        for (int i = 0; i < HEATING_ROOMS; i++) {
            int16_t tenth; bool stale;
            if (heating_room_current(heating_room(i), &tenth, &stale, NULL) && !stale) live++;
        }
        snprintf(buf, sizeof(buf), "Online - %d of %d rooms reporting      %s",
                 live, HEATING_ROOMS, clock);
    }
    lv_label_set_text(s_footer, buf);
}

static void paint_banner(void)
{
    int32_t left = (int32_t)(s_banner_until - (int64_t)now_ms());
    if (s_banner_text[0] != 0 && left > 0) {
        lv_label_set_text(s_banner, s_banner_text);
        lv_obj_remove_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_banner, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_listening) {
        lv_obj_remove_flag(s_listen_box, LV_OBJ_FLAG_HIDDEN);
        int fraction = s_listen_s < 0 ? 0 : (s_listen_s > LISTEN_MS / 1000 ? LISTEN_MS / 1000 : s_listen_s);
        int width = (CARD_W * 2 + GRID_GAP_X) * fraction / (LISTEN_MS / 1000);
        lv_obj_set_width(s_listen_bar, width > 0 ? width : 1);
    } else {
        lv_obj_add_flag(s_listen_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void paint_dimming(void)
{
    /* Low brightness after two idle minutes; any touch or button restores it. */
    bool want_dim = (now_ms() - (uint32_t)s_last_activity) > DIM_AFTER_MS;
    if (want_dim == s_dimmed) return;
    s_dimmed = want_dim;
    for (int i = 0; i < HEATING_ROOMS; i++) {
        lv_obj_set_style_opa(s_cards[i].card, want_dim ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
    lv_obj_set_style_opa(s_footer, want_dim ? LV_OPA_40 : LV_OPA_COVER, 0);
}

static void paint_all(void)
{
    s_online_ui = heating_online();
    for (int i = 0; i < HEATING_ROOMS; i++) paint_card(i);
    paint_footer();
    paint_banner();
    paint_dimming();
}

/* ── the LVGL timer ──────────────────────────────────────────────────── */

/* Timers (banner expiry, listening bar, the clock, the dim decision) and the
 * refresh flag all live in one LVGL timer: a FreeRTOS task cannot paint when
 * the framework started LVGL on its own task, so the LVGL task does it here. */
static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    static int64_t next_clock = 0;

    if (!s_ui_ready) return;

    int64_t clock_now = esp_timer_get_time() / 1000;
    bool online_now = heating_online();
    bool dirties = s_refresh || s_banner_dirty || (online_now != s_online_ui) ||
                   (clock_now - next_clock) > 1000;
    if (s_listening) dirties = true;
    if (s_rejecting && (int32_t)(s_reject_until - (int64_t)now_ms()) <= 0) {
        s_rejecting = false;
        dirties = true;
    }

    if (!dirties) return;

    s_refresh = false;
    s_banner_dirty = false;
    if (clock_now - next_clock > 1000) next_clock = clock_now;

    /* Timer callbacks run on the LVGL task: the lock is already held, and
     * taking it again would deadlock. */
    paint_all();
}

/* The listening countdown: ten ticks of one second while the window is open. */
static void listen_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ui_ready && s_listening) {
        heating_voice_seconds_left(s_listen_s > 0 ? s_listen_s - 1 : 0);
    }
}

/**
 * The display framework already registered the board's GT1151 through
 * lvgl_port_add_touch(), so that input device is the ONLY reader of the touch
 * controller — a second reader would deliver every tap twice and a selection
 * tap would turn into a setpoint change.
 *
 * LVGL drains that device in its own read phase and leaves it in RELEASED, so
 * its state cannot be sampled afterwards. The tap is therefore taken from
 * LVGL's own press/release callbacks on the display, which fire inside that
 * same read phase: a press records the point, a release is the tap. Registered
 * on the display object so a touch that lands on our widgets (or anywhere
 * else) is seen either way.
 */
static void on_touch_press(lv_event_t *e)
{
    (void)e;
    if (s_touch_indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(s_touch_indev, &p);
    if (p.x < 0 || p.y < 0) return;
    s_touch_x = p.x;
    s_touch_y = p.y;
    touch_activity();
}

static void on_touch_release(lv_event_t *e)
{
    (void)e;
    touch_hit(s_touch_x, s_touch_y);       /* released: that is the tap */
}

esp_err_t heating_ui_init(void)
{
    esp_err_t err = app_display_init();
    if (err != ESP_OK) return err;

    s_last_activity = now_ms();

    /* app_display_init() starts LVGL, the panel and the touch in ITS OWN task.
     * Waiting a fixed 1.5 s and then taking the LVGL lock raced that task on
     * real hardware (the log showed the second bring-up aborting right after
     * "Panel 'display_lcd' …"): wait for the display to report ready instead,
     * with a bounded timeout, and only then touch LVGL. */
    bool ready = false;
    for (int i = 0; i < 300; i++) {              /* up to 30 s */
        if (app_display_ready() || app_display_failed()) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ready = app_display_ready();
    if (!ready) {
        ESP_LOGE(TAG, "no panel from the display framework — cards unavailable");
        return ESP_ERR_TIMEOUT;                  /* the panel keeps running */
    }

    if (!lvgl_port_lock(5000)) {
        ESP_LOGE(TAG, "LVGL is not running — no dashboard");
        return ESP_ERR_TIMEOUT;
    }
    build_screen();
    paint_all();

    /* The framework's pointer device, if its touch came up. */
    for (lv_indev_t *i = lv_indev_get_next(NULL); i != NULL; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
            s_touch_indev = i;
            break;
        }
    }
    if (s_touch_indev == NULL) {
        ESP_LOGW(TAG, "no touch input device — cards selectable, −/+ from voice only");
    } else {
        lv_obj_add_event_cb(s_screen, on_touch_press, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_screen, on_touch_release, LV_EVENT_RELEASED, NULL);
    }

    lv_timer_create(ui_timer_cb, 250, NULL);
    lv_timer_create(listen_timer_cb, 1000, NULL);
    s_ui_ready = true;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "dashboard up (%d rooms)", HEATING_ROOMS);
    return ESP_OK;
}

/* Serial redraw hook, used by the front button's press feedback. */
void heating_ui_touch_activity(void)
{
    touch_activity();
    s_refresh = true;
}

bool heating_ui_has_touch(void)
{
    return s_touch_indev != NULL;
}
