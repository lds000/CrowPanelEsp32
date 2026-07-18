/**
 * ui.cpp — Main dashboard: idle weather scene + active watering scene
 *
 * Navigation:
 *   Idle → touch → controls appear (zone chips, HISTORY, SCHEDULE buttons)
 *   Zone chip → duration picker modal → starts run
 *   HISTORY/SCHEDULE → separate full screens (built in ui_history.cpp / ui_schedule.cpp)
 */

#include <lvgl.h>
#include <Arduino.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "ui_theme.h"
#include "lawnbot_images.h"

/* Declared in lv_conf.h */
extern const lv_font_t font_clock_120;

/* Forward declarations to other UI screens */
void ui_schedule_build();
void ui_schedule_refresh();
void ui_history_build();
void ui_history_refresh();
void ui_show_toast(const char *text, uint32_t ms);

/* ═══════════════════════════════════════════════════════════
   ENUMS / HELPERS
═══════════════════════════════════════════════════════════ */
enum WeatherCondition {
    WEATHER_UNKNOWN, WEATHER_WINDY, WEATHER_FREEZING
};

static bool weather_is_stale();
static bool status_is_stale();

static int parse_hour(const char *s) {
    int h = 12, m = 0, sec = 0;
    if (s && sscanf(s, "%d:%d:%d", &h, &m, &sec) >= 1) return h;
    return 12;
}

static bool minute_changed(const char *a, const char *b) {
    return strncmp(a ? a : "", b ? b : "", 5) != 0;
}

static void format_hhmm(char *out, size_t n) {
    if (g_state.time_str[0] && strlen(g_state.time_str) >= 5)
        snprintf(out, n, "%.5s", g_state.time_str);
    else
        snprintf(out, n, "--:--");
}

static bool is_night() { int h = parse_hour(g_state.time_str); return h < 6 || h >= 20; }

static WeatherCondition get_condition() {
    if (!g_state.weather.valid || weather_is_stale()) return WEATHER_UNKNOWN;
    if (g_state.weather.wind_speed_valid && g_state.weather.wind_mph > 25.0f)
        return WEATHER_WINDY;
    if (g_state.weather.temp_valid && g_state.weather.temp_f < 32.0f)
        return WEATHER_FREEZING;
    return WEATHER_UNKNOWN;
}

static const char *condition_label(WeatherCondition c) {
    switch (c) {
        case WEATHER_WINDY:  return "WINDY";
        case WEATHER_FREEZING: return "FREEZING TEMPERATURE";
        default: return g_state.weather.valid ? "WEATHER SENSORS ONLINE"
                                             : "WAITING FOR WEATHER";
    }
}

static bool relay_for_zone_on(const char *zone) {
    if (strcmp(zone, "Hanging Pots") == 0) return g_state.relays.hanging_pots;
    if (strcmp(zone, "Garden") == 0) return g_state.relays.garden;
    if (strcmp(zone, "Misters") == 0) return g_state.relays.misters;
    return g_state.current_run.active;
}

static void fmt_mmss(int sec, char *out, size_t n) {
    if (sec < 0) sec = 0;
    snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

static void fmt_minutes_label(int sec, char *out, size_t n) {
    if (sec < 0) sec = 0;
    int min = (sec + 59) / 60;
    snprintf(out, n, "%d MIN", min);
}

static void fmt_end_time(int rem_sec, char *out, size_t n) {
    int h = 0, m = 0, s = 0;
    if (sscanf(g_state.time_str, "%d:%d:%d", &h, &m, &s) < 2) {
        snprintf(out, n, "--:--");
        return;
    }
    int total = h * 3600 + m * 60 + s + rem_sec;
    total = ((total % 86400) + 86400) % 86400;
    snprintf(out, n, "%02d:%02d", total / 3600, (total / 60) % 60);
}

static void clear_chrome(lv_obj_t *o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

static void set_hidden(lv_obj_t *o, bool hidden) {
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_disabled(lv_obj_t *o, bool disabled) {
    if (!o) return;
    if (disabled) lv_obj_add_state(o, LV_STATE_DISABLED);
    else          lv_obj_clear_state(o, LV_STATE_DISABLED);
}

static bool timestamp_stale(uint32_t updated_ms, uint32_t max_age_ms) {
    return updated_ms == 0 || (uint32_t)(millis() - updated_ms) > max_age_ms;
}

static bool status_is_stale() {
    return !g_state.demo_mode &&
           (!g_state.ws_connected || timestamp_stale(g_state.status_updated_ms,
                                                     UI_STATUS_STALE_MS));
}

static bool weather_is_stale() {
    return !g_state.demo_mode && timestamp_stale(g_state.weather_updated_ms,
                                                 UI_WEATHER_STALE_MS);
}

static bool irrigation_is_stale() {
    return !g_state.demo_mode && timestamp_stale(g_state.irrigation.updated_ms,
                                                 UI_WEATHER_STALE_MS);
}

static lv_obj_t *make_circle(lv_obj_t *p, int x, int y, int sz, uint32_t col, lv_opa_t opa) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, sz, sz);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_cloud(lv_obj_t *p, int x, int y, int s) {
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_size(c, 180*s/100, 90*s/100);
    clear_chrome(c);
    make_circle(c,  10*s/100, 30*s/100, 42*s/100, 0xF2F6FBu, LV_OPA_80);
    make_circle(c,  45*s/100, 10*s/100, 56*s/100, 0xF2F6FBu, LV_OPA_90);
    make_circle(c,  90*s/100, 24*s/100, 48*s/100, 0xF2F6FBu, LV_OPA_80);
    lv_obj_t *b = lv_obj_create(c);
    lv_obj_set_pos(b, 26*s/100, 36*s/100);
    lv_obj_set_size(b, 108*s/100, 30*s/100);
    lv_obj_set_style_radius(b, 18*s/100, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xF2F6FBu), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *make_rain(lv_obj_t *p, int x, int y) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, 4, 24);
    lv_obj_set_style_radius(o, 2, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x8FD6FFu), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    lv_obj_set_style_transform_angle(o, 200, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *make_wind(lv_obj_t *p, int x, int y, int w) {
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, 3);
    lv_obj_set_style_radius(o, 3, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0xBFD8FFu), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* ═══════════════════════════════════════════════════════════
   GLOBAL OBJECTS
═══════════════════════════════════════════════════════════ */
lv_obj_t *g_dash_screen   = nullptr;
lv_obj_t *g_splash_screen = nullptr;
static lv_obj_t *g_splash_status = nullptr;

/* Background */
static lv_obj_t *g_bg_layer, *g_celestial, *g_cloud1, *g_cloud2;
static lv_obj_t *g_star1, *g_star2, *g_star3, *g_star4;
static lv_obj_t *g_rain1, *g_rain2, *g_rain3, *g_rain4;
static lv_obj_t *g_wind1, *g_wind2, *g_wind3, *g_bg_dim;

/* Header */
static lv_obj_t *g_brand_img, *g_sub_lbl, *g_mode_lbl;

/* Idle content */
static lv_obj_t *g_idle_grp, *g_idle_time, *g_idle_date;
static lv_obj_t *g_idle_cond, *g_idle_meta, *g_idle_next;

/* Active content */
static lv_obj_t *g_act_grp, *g_act_zone, *g_act_count, *g_act_sub, *g_act_bar;
static lv_obj_t *g_act_elapsed, *g_act_total, *g_act_end, *g_act_flow, *g_act_mode;
static lv_obj_t *g_act_pulse, *g_act_stop_btn, *g_act_stop_lbl;
static CurrentRun g_act_last_raw = {};
static uint32_t   g_act_anchor_ms = 0;
static int        g_act_anchor_rem = 0;

/* Controls bar (130 px at bottom) */
#define CTRL_H 116
static lv_obj_t *g_ctrl_grp, *g_hint_lbl, *g_stop_btn, *g_stop_lbl;
static lv_obj_t *g_hist_btn,  *g_sched_btn;
struct ZoneChip { lv_obj_t *btn; lv_obj_t *lbl; };
static ZoneChip g_chips[3];

/* Toast */
static lv_obj_t *g_toast = nullptr;
static lv_obj_t *g_toast_lbl = nullptr;
static uint32_t  g_toast_hide_at = 0;

/* Duration picker modal */
static lv_obj_t *g_picker_panel = nullptr;
static lv_obj_t *g_picker_title = nullptr;
static int       g_picker_zone  = -1;
static const int DURATIONS[]    = {1, 3, 5, 10, 15, 20};
static lv_obj_t *g_dur_btns[6];

/* ═══════════════════════════════════════════════════════════
   DURATION PICKER EVENTS
═══════════════════════════════════════════════════════════ */
static void on_duration(lv_event_t *e) {
    int min = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_pending.type != PENDING_NONE || g_state.action.busy) return;
    if (!g_state.demo_mode && (!g_state.controls_auth_configured || status_is_stale())) {
        set_hidden(g_picker_panel, true);
        g_picker_zone = -1;
        ui_show_toast("CONTROLS UNAVAILABLE", 2500);
        return;
    }
    if (g_picker_zone >= 0 && g_picker_zone < 3) {
        strncpy(g_pending.zone_name, ZONE_API_NAMES[g_picker_zone],
                sizeof(g_pending.zone_name) - 1);
        g_pending.run_minutes = min;
        g_pending.type        = PENDING_RUN_ZONE;
    }
    set_hidden(g_picker_panel, true);
    g_picker_zone = -1;
}

static void on_picker_cancel(lv_event_t * /*e*/) {
    set_hidden(g_picker_panel, true);
    g_picker_zone = -1;
}

static void show_duration_picker(int zone_idx) {
    g_picker_zone = zone_idx;
    char title[40];
    snprintf(title, sizeof(title), "START  %s", ZONE_DISPLAY_NAMES[zone_idx]);
    lv_label_set_text(g_picker_title, title);
    lv_obj_clear_flag(g_picker_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_picker_panel);
}

/* ═══════════════════════════════════════════════════════════
   CONTROL CALLBACKS
═══════════════════════════════════════════════════════════ */
static void on_stop(lv_event_t * /*e*/) {
    if (!g_state.demo_mode && !g_state.controls_auth_configured) {
        ui_show_toast("CONTROL TOKEN NOT CONFIGURED", 2500);
        return;
    }
    const bool stop_in_progress = g_pending.type == PENDING_STOP_ALL ||
        (g_state.action.busy && g_state.action.type == PENDING_STOP_ALL);
    if (stop_in_progress) return;

    /* STOP may replace a command that loop() has not consumed yet and may
       supersede a reported request already owned by the network worker. */
    g_pending.zone_name[0] = '\0';
    g_pending.run_minutes = 0;
    g_pending.type = PENDING_STOP_ALL;
}

static void on_zone_chip(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) return;
    if (g_pending.type != PENDING_NONE || g_state.action.busy) return;
    if (!g_state.demo_mode && !g_state.controls_auth_configured) {
        ui_show_toast("CONTROL TOKEN NOT CONFIGURED", 2500);
        return;
    }
    if (g_state.current_run.active) {
        g_pending.type = PENDING_STOP_ALL;
        return;
    }
    show_duration_picker(idx);
}

static void on_history_btn(lv_event_t * /*e*/) {
    if (g_pending.type != PENDING_NONE || g_state.action.busy) return;
    g_state.active_screen = 2;
    if (!g_state.demo_mode) g_pending.type = PENDING_FETCH_HISTORY;
    ui_history_build();
}

static void on_schedule_btn(lv_event_t * /*e*/) {
    if (g_pending.type != PENDING_NONE || g_state.action.busy) return;
    g_state.active_screen = 1;
    if (!g_state.demo_mode) g_pending.type = PENDING_FETCH_SCHEDULE;
    ui_schedule_build();
}

/* ═══════════════════════════════════════════════════════════
   SCENE BUILDERS
═══════════════════════════════════════════════════════════ */
static void create_background(lv_obj_t *scr) {
    g_bg_layer = lv_obj_create(scr);
    lv_obj_set_size(g_bg_layer, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_bg_layer, 0, 0);
    lv_obj_set_style_bg_opa(g_bg_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_bg_layer, 0, 0);
    lv_obj_set_style_radius(g_bg_layer, 0, 0);
    lv_obj_clear_flag(g_bg_layer, LV_OBJ_FLAG_SCROLLABLE);

    g_celestial = make_circle(g_bg_layer, 560, 54, 96, 0xFFD56Au, LV_OPA_90);
    g_cloud1    = make_cloud(g_bg_layer,  80, 70, 100);
    g_cloud2    = make_cloud(g_bg_layer, 480, 130, 84);
    g_star1     = make_circle(g_bg_layer, 118, 48,  5, 0xF6F8FCu, LV_OPA_90);
    g_star2     = make_circle(g_bg_layer, 160, 86,  4, 0xF6F8FCu, LV_OPA_80);
    g_star3     = make_circle(g_bg_layer, 650, 30,  5, 0xF6F8FCu, LV_OPA_90);
    g_star4     = make_circle(g_bg_layer, 720, 88,  4, 0xF6F8FCu, LV_OPA_80);
    g_rain1     = make_rain(g_bg_layer, 160, 184);
    g_rain2     = make_rain(g_bg_layer, 210, 202);
    g_rain3     = make_rain(g_bg_layer, 530, 210);
    g_rain4     = make_rain(g_bg_layer, 575, 224);
    g_wind1     = make_wind(g_bg_layer, 108, 212, 96);
    g_wind2     = make_wind(g_bg_layer, 540, 192, 84);
    g_wind3     = make_wind(g_bg_layer, 596, 230, 62);

    g_bg_dim = lv_obj_create(scr);
    lv_obj_set_size(g_bg_dim, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_bg_dim, 0, 0);
    lv_obj_set_style_bg_color(g_bg_dim, lv_color_hex(C_IDLE_MASK), 0);
    lv_obj_set_style_bg_opa(g_bg_dim, LV_OPA_20, 0);
    lv_obj_set_style_border_width(g_bg_dim, 0, 0);
    lv_obj_set_style_radius(g_bg_dim, 0, 0);
    lv_obj_clear_flag(g_bg_dim, LV_OBJ_FLAG_SCROLLABLE);
}

static void create_header(lv_obj_t *scr) {
    /* Robot mascot moved into the open left-side space and scaled up. */
    lv_obj_t *robot = lv_img_create(scr);
    lv_img_set_src(robot, &img_robot_48);
    lv_img_set_zoom(robot, 416);
    lv_obj_set_pos(robot, 18, 278);
    lv_obj_set_style_img_opa(robot, LV_OPA_90, 0);

    /* Wordmark floats directly on the background with no boxed panel behind it. */
    g_brand_img = lv_img_create(scr);
    lv_img_set_src(g_brand_img, &img_title_header);
    lv_img_set_zoom(g_brand_img, 300);
    lv_obj_set_pos(g_brand_img, 82, 10);

    g_sub_lbl = lv_label_create(scr);
    lv_label_set_text(g_sub_lbl, "BOISE STATE EDITION");
    lv_obj_set_style_text_font(g_sub_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_sub_lbl, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_text_letter_space(g_sub_lbl, 2, 0);
    lv_obj_align_to(g_sub_lbl, g_brand_img, LV_ALIGN_OUT_BOTTOM_LEFT, 8, -2);

    g_mode_lbl = lv_label_create(scr);
    lv_label_set_text(g_mode_lbl, "DEMO MODE");
    lv_obj_set_style_text_font(g_mode_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_align(g_mode_lbl, LV_ALIGN_TOP_RIGHT, -24, 24);
}

static void create_idle_group(lv_obj_t *scr) {
    g_idle_grp = lv_obj_create(scr);
    lv_obj_set_size(g_idle_grp, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_idle_grp, 0, 0);
    clear_chrome(g_idle_grp);

    /* Big clock — baked 120pt Arial Bold bitmap font */
    g_idle_time = lv_label_create(g_idle_grp);
    lv_label_set_text(g_idle_time, "--:--");
    lv_obj_set_width(g_idle_time, 560);
    lv_obj_set_style_text_align(g_idle_time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_idle_time, &font_clock_120, 0);
    lv_obj_set_style_text_color(g_idle_time, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_letter_space(g_idle_time, 4, 0);
    lv_obj_set_style_bg_opa(g_idle_time, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_idle_time, 0, 0);
    lv_obj_set_style_pad_all(g_idle_time, 0, 0);
    lv_obj_align(g_idle_time, LV_ALIGN_TOP_MID, 0, 80);

    /* Date */
    g_idle_date = lv_label_create(g_idle_grp);
    lv_label_set_text(g_idle_date, "---");
    lv_obj_set_width(g_idle_date, 560);
    lv_obj_set_style_text_align(g_idle_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_idle_date, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_idle_date, lv_color_hex(C_MUTED), 0);
    lv_obj_align(g_idle_date, LV_ALIGN_TOP_MID, 0, 240);

    /* Condition */
    g_idle_cond = lv_label_create(g_idle_grp);
    lv_label_set_text(g_idle_cond, "WAITING FOR WEATHER");
    lv_obj_set_width(g_idle_cond, 560);
    lv_obj_set_style_text_align(g_idle_cond, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_idle_cond, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_idle_cond, lv_color_hex(C_TEXT), 0);
    lv_obj_align(g_idle_cond, LV_ALIGN_TOP_MID, 0, 278);

    /* Meta (temp/humidity/wind) */
    g_idle_meta = lv_label_create(g_idle_grp);
    lv_label_set_text(g_idle_meta, "74 F  |  HUM 52%  |  WIND 5 MPH NW");
    lv_obj_set_width(g_idle_meta, 660);
    lv_obj_set_height(g_idle_meta, 22);
    lv_label_set_long_mode(g_idle_meta, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(g_idle_meta, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_idle_meta, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_idle_meta, lv_color_hex(C_MUTED), 0);
    lv_obj_align(g_idle_meta, LV_ALIGN_TOP_MID, 0, 314);

    /* Next run */
    g_idle_next = lv_label_create(g_idle_grp);
    lv_label_set_text(g_idle_next, "NEXT: GARDEN  07:00");
    lv_obj_set_width(g_idle_next, 560);
    lv_obj_set_style_text_align(g_idle_next, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_idle_next, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_idle_next, lv_color_hex(C_ORANGE), 0);
    lv_obj_align(g_idle_next, LV_ALIGN_TOP_MID, 0, 340);
}

static lv_obj_t *create_active_stat(lv_obj_t *parent, int x, const char *caption) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 178, 76);
    lv_obj_set_pos(box, x, 326);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x07182Cu), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_80, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cap = lv_label_create(box);
    lv_label_set_text(cap, caption);
    lv_obj_set_width(cap, 158);
    lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(cap, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(cap, lv_color_hex(C_MUTED), 0);
    lv_obj_set_pos(cap, 10, 8);

    lv_obj_t *val = lv_label_create(box);
    lv_label_set_text(val, "--");
    lv_obj_set_width(val, 158);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(val, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(val, 10, 38);
    return val;
}

static void create_active_group(lv_obj_t *scr) {
    g_act_grp = lv_obj_create(scr);
    lv_obj_set_size(g_act_grp, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_act_grp, 0, 0);
    lv_obj_set_style_bg_color(g_act_grp, lv_color_hex(0x020A12u), 0);
    lv_obj_set_style_bg_grad_color(g_act_grp, lv_color_hex(0x102445u), 0);
    lv_obj_set_style_bg_grad_dir(g_act_grp, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_act_grp, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_act_grp, 0, 0);
    lv_obj_set_style_radius(g_act_grp, 0, 0);
    lv_obj_clear_flag(g_act_grp, LV_OBJ_FLAG_SCROLLABLE);

    g_act_mode = lv_label_create(g_act_grp);
    lv_label_set_text(g_act_mode, "WATERING NOW");
    lv_obj_set_width(g_act_mode, 280);
    lv_obj_set_style_text_font(g_act_mode, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_act_mode, lv_color_hex(0x7EF082u), 0);
    lv_obj_set_pos(g_act_mode, 28, 20);

    g_act_flow = lv_label_create(g_act_grp);
    lv_label_set_text(g_act_flow, "VALVE: ON");
    lv_obj_set_size(g_act_flow, 286, 48);
    lv_label_set_long_mode(g_act_flow, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(g_act_flow, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(g_act_flow, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_act_flow, lv_color_hex(0x7EF082u), 0);
    lv_obj_set_pos(g_act_flow, 480, 10);

    g_act_pulse = lv_obj_create(g_act_grp);
    lv_obj_set_size(g_act_pulse, 28, 28);
    lv_obj_set_pos(g_act_pulse, 446, 21);
    lv_obj_set_style_radius(g_act_pulse, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_act_pulse, lv_color_hex(0x7EF082u), 0);
    lv_obj_set_style_bg_opa(g_act_pulse, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_act_pulse, 0, 0);
    lv_obj_clear_flag(g_act_pulse, LV_OBJ_FLAG_SCROLLABLE);

    g_act_zone = lv_label_create(g_act_grp);
    lv_label_set_text(g_act_zone, "GARDEN");
    lv_obj_set_width(g_act_zone, 760);
    lv_obj_set_style_text_align(g_act_zone, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_act_zone, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(g_act_zone, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_letter_space(g_act_zone, 2, 0);
    lv_obj_set_pos(g_act_zone, 20, 74);

    g_act_count = lv_label_create(g_act_grp);
    lv_label_set_text(g_act_count, "4:59");
    lv_obj_set_width(g_act_count, 760);
    lv_obj_set_style_text_align(g_act_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_act_count, &font_clock_120, 0);
    lv_obj_set_style_text_color(g_act_count, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_text_letter_space(g_act_count, 4, 0);
    lv_obj_set_pos(g_act_count, 20, 128);

    g_act_sub = lv_label_create(g_act_grp);
    lv_label_set_text(g_act_sub, "REMAINING");
    lv_obj_set_width(g_act_sub, 760);
    lv_obj_set_style_text_align(g_act_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_act_sub, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_act_sub, lv_color_hex(C_MUTED), 0);
    lv_obj_set_pos(g_act_sub, 20, 246);

    g_act_bar = lv_bar_create(g_act_grp);
    lv_obj_set_size(g_act_bar, 720, 24);
    lv_obj_set_pos(g_act_bar, 40, 286);
    lv_bar_set_range(g_act_bar, 0, 100);
    lv_bar_set_value(g_act_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(g_act_bar, lv_color_hex(C_IDLE_MASK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_act_bar, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_act_bar, lv_color_hex(C_ORANGE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g_act_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(g_act_bar, 12, LV_PART_INDICATOR);

    g_act_elapsed = create_active_stat(g_act_grp,  28, "ELAPSED");
    g_act_total   = create_active_stat(g_act_grp, 216, "TOTAL");
    g_act_end     = create_active_stat(g_act_grp, 404, "ENDS");

    g_act_stop_btn = lv_btn_create(g_act_grp);
    lv_obj_set_size(g_act_stop_btn, 174, 76);
    lv_obj_set_pos(g_act_stop_btn, 592, 326);
    lv_obj_set_style_bg_color(g_act_stop_btn, lv_color_hex(C_DANGER), 0);
    lv_obj_set_style_bg_color(g_act_stop_btn, lv_color_hex(0xB53936u), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(g_act_stop_btn, 0, 0);
    lv_obj_set_style_radius(g_act_stop_btn, 10, 0);
    lv_obj_add_event_cb(g_act_stop_btn, on_stop, LV_EVENT_CLICKED, nullptr);
    g_act_stop_lbl = lv_label_create(g_act_stop_btn);
    lv_label_set_text(g_act_stop_lbl, "STOP");
    lv_obj_set_style_text_font(g_act_stop_lbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(g_act_stop_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(g_act_stop_lbl);

    lv_obj_t *stop_hint = lv_label_create(g_act_grp);
    lv_label_set_text(stop_hint, "EMERGENCY STOP");
    lv_obj_set_width(stop_hint, 174);
    lv_obj_set_style_text_align(stop_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(stop_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(stop_hint, lv_color_hex(C_MUTED), 0);
    lv_obj_set_pos(stop_hint, 592, 408);
}

/* Controls bar ─ 130 px strip at bottom of screen.
 *
 * Layout when controls visible:
 *   [HISTORY]  [POTS]  [GARDEN]  [MISTERS]  [SCHEDULE]
 *   or (if active):
 *   [HISTORY]  [    STOP ALL WATERING    ]  [SCHEDULE]
 *
 * Layout when controls hidden: centered "TOUCH SCREEN FOR CONTROLS" hint.
 *
 * X positions (from left):
 *   HISTORY  : x=20,  w=120
 *   Chip 0   : x=157, w=152
 *   Chip 1   : x=317, w=152
 *   Chip 2   : x=477, w=152
 *   SCHEDULE : x=660, w=120
 */
static void create_controls_group(lv_obj_t *scr) {
    g_ctrl_grp = lv_obj_create(scr);
    lv_obj_set_size(g_ctrl_grp, SCREEN_W, CTRL_H);
    lv_obj_set_pos(g_ctrl_grp, 0, SCREEN_H - CTRL_H);
    clear_chrome(g_ctrl_grp);

    /* Touch hint */
    g_hint_lbl = lv_label_create(g_ctrl_grp);
    lv_label_set_text(g_hint_lbl, "TOUCH SCREEN FOR CONTROLS");
    lv_obj_set_style_text_font(g_hint_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_hint_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_align(g_hint_lbl, LV_ALIGN_CENTER, 0, 0);

    /* STOP ALL button */
    g_stop_btn = lv_btn_create(g_ctrl_grp);
    lv_obj_set_size(g_stop_btn, 360, 54);
    lv_obj_set_pos(g_stop_btn, 220, 24);
    lv_obj_set_style_bg_color(g_stop_btn, lv_color_hex(C_DANGER), 0);
    lv_obj_set_style_bg_color(g_stop_btn, lv_color_hex(0xB53936u), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(g_stop_btn, 0, 0);
    lv_obj_set_style_radius(g_stop_btn, 16, 0);
    lv_obj_add_event_cb(g_stop_btn, on_stop, LV_EVENT_CLICKED, nullptr);
    g_stop_lbl = lv_label_create(g_stop_btn);
    lv_label_set_text(g_stop_lbl, "STOP ALL WATERING");
    lv_obj_set_style_text_font(g_stop_lbl, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_stop_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(g_stop_lbl);

    /* Zone chips */
    static const int CHIP_X[3] = {157, 317, 477};
    for (int i = 0; i < 3; i++) {
        g_chips[i].btn = lv_btn_create(g_ctrl_grp);
        lv_obj_set_size(g_chips[i].btn, 152, 54);
        lv_obj_set_pos(g_chips[i].btn, CHIP_X[i], 24);
        lv_obj_set_style_bg_color(g_chips[i].btn, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_bg_opa(g_chips[i].btn, LV_OPA_90, 0);
        lv_obj_set_style_border_color(g_chips[i].btn, lv_color_hex(C_PANEL_EDGE), 0);
        lv_obj_set_style_border_width(g_chips[i].btn, 2, 0);
        lv_obj_set_style_radius(g_chips[i].btn, 16, 0);
        lv_obj_add_event_cb(g_chips[i].btn, on_zone_chip, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        g_chips[i].lbl = lv_label_create(g_chips[i].btn);
        lv_label_set_text(g_chips[i].lbl, ZONE_DISPLAY_NAMES[i]);
        lv_obj_set_style_text_font(g_chips[i].lbl, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(g_chips[i].lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_center(g_chips[i].lbl);
    }

    /* HISTORY nav button */
    g_hist_btn = lv_btn_create(g_ctrl_grp);
    lv_obj_set_size(g_hist_btn, 120, 54);
    lv_obj_set_pos(g_hist_btn, 20, 24);
    lv_obj_set_style_bg_color(g_hist_btn, lv_color_hex(0x0A2240u), 0);
    lv_obj_set_style_bg_opa(g_hist_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_color(g_hist_btn, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_border_width(g_hist_btn, 2, 0);
    lv_obj_set_style_radius(g_hist_btn, 16, 0);
    lv_obj_add_event_cb(g_hist_btn, on_history_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *hl = lv_label_create(g_hist_btn);
    lv_label_set_text(hl, "HISTORY");
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(C_ORANGE), 0);
    lv_obj_center(hl);

    /* SCHEDULE nav button */
    g_sched_btn = lv_btn_create(g_ctrl_grp);
    lv_obj_set_size(g_sched_btn, 120, 54);
    lv_obj_set_pos(g_sched_btn, 660, 24);
    lv_obj_set_style_bg_color(g_sched_btn, lv_color_hex(0x0A2240u), 0);
    lv_obj_set_style_bg_opa(g_sched_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_color(g_sched_btn, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_border_width(g_sched_btn, 2, 0);
    lv_obj_set_style_radius(g_sched_btn, 16, 0);
    lv_obj_add_event_cb(g_sched_btn, on_schedule_btn, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(g_sched_btn);
    lv_label_set_text(sl, "SCHEDULE");
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(C_ORANGE), 0);
    lv_obj_center(sl);
}

static void create_toast(lv_obj_t *scr) {
    g_toast = lv_obj_create(scr);
    lv_obj_set_size(g_toast, 440, 42);
    lv_obj_align(g_toast, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_bg_color(g_toast, lv_color_hex(0x081628u), 0);
    lv_obj_set_style_bg_opa(g_toast, LV_OPA_90, 0);
    lv_obj_set_style_border_color(g_toast, lv_color_hex(C_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(g_toast, 1, 0);
    lv_obj_set_style_radius(g_toast, 16, 0);
    lv_obj_set_style_pad_all(g_toast, 8, 0);
    lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);

    g_toast_lbl = lv_label_create(g_toast);
    lv_obj_set_width(g_toast_lbl, 408);
    lv_label_set_long_mode(g_toast_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(g_toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_toast_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_toast_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(g_toast_lbl);
}

/* Duration picker modal — overlay panel, initially hidden */
static void create_duration_picker(lv_obj_t *scr) {
    /* Semi-transparent backdrop */
    g_picker_panel = lv_obj_create(scr);
    lv_obj_set_size(g_picker_panel, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_picker_panel, 0, 0);
    lv_obj_set_style_bg_color(g_picker_panel, lv_color_hex(0x000000u), 0);
    lv_obj_set_style_bg_opa(g_picker_panel, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_picker_panel, 0, 0);
    lv_obj_set_style_radius(g_picker_panel, 0, 0);
    lv_obj_clear_flag(g_picker_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_picker_panel, LV_OBJ_FLAG_HIDDEN);

    /* Card */
    lv_obj_t *card = lv_obj_create(g_picker_panel);
    lv_obj_set_size(card, 680, 300);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    g_picker_title = lv_label_create(card);
    lv_label_set_text(g_picker_title, "START ZONE");
    lv_obj_set_style_text_font(g_picker_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(g_picker_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_letter_space(g_picker_title, 2, 0);
    lv_obj_align(g_picker_title, LV_ALIGN_TOP_MID, 0, 18);

    /* Duration buttons — 2 rows of 3 */
    static const int DX[6] = {40, 260, 480, 40, 260, 480};
    static const int DY[6] = {72,  72,  72, 144, 144, 144};
    for (int i = 0; i < 6; i++) {
        g_dur_btns[i] = lv_btn_create(card);
        lv_obj_set_size(g_dur_btns[i], 160, 52);
        lv_obj_set_pos(g_dur_btns[i], DX[i], DY[i]);
        lv_obj_set_style_bg_color(g_dur_btns[i], lv_color_hex(C_BLUE), 0);
        lv_obj_set_style_bg_color(g_dur_btns[i], lv_color_hex(C_ORANGE), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(g_dur_btns[i], 0, 0);
        lv_obj_set_style_radius(g_dur_btns[i], 14, 0);
        lv_obj_add_event_cb(g_dur_btns[i], on_duration, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(DURATIONS[i])));
        char lbl[12];
        snprintf(lbl, sizeof(lbl), "%d MIN", DURATIONS[i]);
        lv_obj_t *lb = lv_label_create(g_dur_btns[i]);
        lv_label_set_text(lb, lbl);
        lv_obj_set_style_text_font(lb, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lb, lv_color_hex(C_TEXT), 0);
        lv_obj_center(lb);
    }

    /* CANCEL button below duration shortcuts */
    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 160, 52);
    lv_obj_set_pos(cancel, 260, 222);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x1A2840u), 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(C_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(cancel, 2, 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, on_picker_cancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "CANCEL");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(C_MUTED), 0);
    lv_obj_center(cl);
}

/* ═══════════════════════════════════════════════════════════
   UPDATERS
═══════════════════════════════════════════════════════════ */
static void update_background() {
    WeatherCondition cond = get_condition();
    bool night = is_night();
    uint32_t top = 0x0E2A5Au, bot = 0x1A6090u;
    if (night)                   { top = 0x04101Fu; bot = 0x0A1E35u; }
    if (cond == WEATHER_WINDY)       { top = 0x0D1E3Au; bot = 0x1A3050u; }
    else if (cond == WEATHER_FREEZING) { top = 0x1A2A50u; bot = 0x2A3A60u; }

    lv_obj_set_style_bg_color(g_bg_layer, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(g_bg_layer, lv_color_hex(bot), 0);
    lv_obj_set_style_bg_grad_dir(g_bg_layer, LV_GRAD_DIR_VER, 0);

    lv_obj_set_style_bg_color(g_celestial, lv_color_hex(night ? 0xDDE8FFu : 0xFFD56Au), 0);
    lv_obj_set_style_bg_opa(g_celestial, night ? LV_OPA_70 : LV_OPA_90, 0);
    lv_obj_set_pos(g_celestial, night ? 600 : 560, night ? 40 : 54);
    lv_obj_set_size(g_celestial, night ? 76 : 96, night ? 76 : 96);

    /* Temperature/humidity do not identify actual sky conditions.  Keep
       decorative clouds/rain hidden until the hub supplies a condition. */
    set_hidden(g_cloud1, true); set_hidden(g_cloud2, true);
    set_hidden(g_rain1, true); set_hidden(g_rain2, true);
    set_hidden(g_rain3, true); set_hidden(g_rain4, true);
    set_hidden(g_wind1, cond != WEATHER_WINDY);
    set_hidden(g_wind2, cond != WEATHER_WINDY);
    set_hidden(g_wind3, cond != WEATHER_WINDY);
    bool stars = night;
    set_hidden(g_star1, !stars); set_hidden(g_star2, !stars);
    set_hidden(g_star3, !stars); set_hidden(g_star4, !stars);

    lv_obj_set_style_bg_opa(g_bg_dim,
        g_state.current_run.active ? LV_OPA_60 : LV_OPA_20, 0);
}

static void update_header() {
    if (g_state.demo_mode) {
        lv_label_set_text(g_mode_lbl, "DEMO MODE");
        lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(C_MUTED), 0);
    } else if (!g_state.wifi_connected) {
        lv_label_set_text(g_mode_lbl, "LIVE  |  WIFI OFFLINE");
        lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(C_DANGER), 0);
    } else if (!g_state.ws_connected) {
        lv_label_set_text(g_mode_lbl, "LIVE  |  HUB SEARCHING");
        lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(C_ORANGE), 0);
    } else if (status_is_stale()) {
        lv_label_set_text(g_mode_lbl, "LIVE  |  HUB DATA STALE");
        lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(C_DANGER), 0);
    } else if (g_state.controls_authenticated) {
        lv_label_set_text(g_mode_lbl, "LIVE  |  CONTROL READY");
        lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(0x7EF082u), 0);
    } else if (g_state.controls_auth_configured) {
        lv_label_set_text(g_mode_lbl, "LIVE  |  CONTROL UNVERIFIED");
        lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(C_ORANGE), 0);
    } else {
        lv_label_set_text(g_mode_lbl, "LIVE  |  CONTROLS LOCKED");
        lv_obj_set_style_text_color(g_mode_lbl, lv_color_hex(C_ORANGE), 0);
    }
}

static void update_idle_group() {
    char meta[160], next_line[64], idle_time[8];
    bool irrigation_fresh = g_state.irrigation.valid && !irrigation_is_stale();
    format_hhmm(idle_time, sizeof(idle_time));
    lv_label_set_text(g_idle_time, idle_time);
    lv_label_set_text(g_idle_date, g_state.date_str[0] ? g_state.date_str : "---");
    lv_label_set_text(g_idle_cond, weather_is_stale()
        ? "WEATHER DATA STALE" : condition_label(get_condition()));

    if (g_state.weather.valid && !weather_is_stale()) {
        uint32_t age_sec = g_state.demo_mode ? 0
            : (uint32_t)(millis() - g_state.weather_updated_ms) / 1000UL;
        char temp[20], humidity[20], wind[32];
        if (g_state.weather.temp_valid)
            snprintf(temp, sizeof(temp), "%.0f F", g_state.weather.temp_f);
        else
            strlcpy(temp, "TEMP --", sizeof(temp));
        if (g_state.weather.humidity_valid)
            snprintf(humidity, sizeof(humidity), "HUM %.0f%%", g_state.weather.humidity_pct);
        else
            strlcpy(humidity, "HUM --", sizeof(humidity));
        if (g_state.weather.wind_speed_valid && g_state.weather.wind_direction_valid)
            snprintf(wind, sizeof(wind), "WIND %.0f MPH %s", g_state.weather.wind_mph,
                     g_state.weather.wind_dir);
        else if (g_state.weather.wind_speed_valid)
            snprintf(wind, sizeof(wind), "WIND %.0f MPH", g_state.weather.wind_mph);
        else if (g_state.weather.wind_direction_valid)
            snprintf(wind, sizeof(wind), "WIND -- MPH %s", g_state.weather.wind_dir);
        else
            strlcpy(wind, "WIND --", sizeof(wind));
        if (irrigation_fresh)
            snprintf(meta, sizeof(meta), "%s  |  %s  |  %s", temp, humidity, wind);
        else
            snprintf(meta, sizeof(meta), "%s  |  %s  |  %s  |  %lus AGO",
                     temp, humidity, wind, (unsigned long)age_sec);
    } else {
        snprintf(meta, sizeof(meta), g_state.weather.valid
            ? "WEATHER DATA STALE" : "WEATHER DATA STANDBY");
    }

    if (irrigation_fresh) {
        size_t used = strlen(meta);
        if (g_state.irrigation.soil_valid && used < sizeof(meta)) {
            snprintf(meta + used, sizeof(meta) - used, "  |  SOIL %.0f%%",
                     g_state.irrigation.soil_moisture_percent);
            used = strlen(meta);
        }
        if (used < sizeof(meta)) {
            if (g_state.irrigation.rainfall_rate_valid)
                snprintf(meta + used, sizeof(meta) - used, "  |  RAIN %.1f MM/H",
                         g_state.irrigation.rainfall_rate_mm_hr);
            else if (g_state.irrigation.rainfall_valid)
                snprintf(meta + used, sizeof(meta) - used, "  |  RAIN %.1f MM",
                         g_state.irrigation.rainfall_mm);
        }
    }
    lv_label_set_text(g_idle_meta, meta);

    if (status_is_stale())
        snprintf(next_line, sizeof(next_line), "NEXT RUN: DATA STALE");
    else if (g_state.next_run.valid)
        snprintf(next_line, sizeof(next_line), "NEXT: %s  %s",
                 zone_display_name(g_state.next_run.zone), g_state.next_run.time_str);
    else
        snprintf(next_line, sizeof(next_line), "NO UPCOMING RUN");
    lv_label_set_text(g_idle_next, next_line);

    set_hidden(g_idle_grp, g_state.current_run.active);
}

static void update_active_group() {
    if (!g_state.current_run.active) {
        g_act_last_raw = {};
        g_act_anchor_ms = 0;
        g_act_anchor_rem = 0;
        set_hidden(g_act_grp, true);
        return;
    }
    lv_obj_clear_flag(g_act_grp, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_act_grp);

    if (memcmp(&g_act_last_raw, &g_state.current_run, sizeof(CurrentRun)) != 0) {
        g_act_last_raw = g_state.current_run;
        g_act_anchor_rem = g_state.current_run.remaining_sec;
        g_act_anchor_ms = millis();
    }

    lv_label_set_text(g_act_zone, zone_display_name(g_state.current_run.zone));

    char count[16], elapsed[16], total_txt[16], end_txt[16];
    int local_elapsed = g_act_anchor_ms ? (int)((millis() - g_act_anchor_ms) / 1000UL) : 0;
    int rem = g_act_anchor_rem - local_elapsed;
    if (rem < 0) rem = 0;
    int total = g_state.current_run.total_sec > 0 ? g_state.current_run.total_sec : g_act_anchor_rem;
    int done = total - rem;
    if (done < 0) done = 0;
    if (done > total) done = total;

    fmt_mmss(rem, count, sizeof(count));
    fmt_mmss(done, elapsed, sizeof(elapsed));
    fmt_minutes_label(total, total_txt, sizeof(total_txt));
    fmt_end_time(rem, end_txt, sizeof(end_txt));

    lv_label_set_text(g_act_count, count);
    lv_label_set_text(g_act_elapsed, elapsed);
    lv_label_set_text(g_act_total, total_txt);
    lv_label_set_text(g_act_end, end_txt);
    lv_label_set_text(g_act_mode, g_state.current_run.is_manual ? "MANUAL WATERING" : "SCHEDULED WATERING");

    bool relay_fresh = !status_is_stale();
    bool flow_on = relay_fresh && relay_for_zone_on(g_state.current_run.zone);
    const char *valve_text = !relay_fresh ? "VALVE: UNKNOWN"
                                          : (flow_on ? "VALVE: ON" : "VALVE: CHECK");
    char flow_text[16], pressure_text[16], telemetry[72];
    bool irrigation_fresh = g_state.irrigation.valid && !irrigation_is_stale();
    if (irrigation_fresh && g_state.irrigation.flow_valid)
        snprintf(flow_text, sizeof(flow_text), "%.1fL/M", g_state.irrigation.flow_rate_lpm);
    else
        strlcpy(flow_text, "--", sizeof(flow_text));
    if (irrigation_fresh && g_state.irrigation.pressure_valid)
        snprintf(pressure_text, sizeof(pressure_text), "%.0fPSI", g_state.irrigation.pressure_psi);
    else
        strlcpy(pressure_text, "--", sizeof(pressure_text));
    snprintf(telemetry, sizeof(telemetry), "%s\nFLOW %s  |  %s",
             valve_text, flow_text, pressure_text);
    lv_label_set_text(g_act_flow, telemetry);
    lv_obj_set_style_text_color(g_act_flow,
        lv_color_hex(!relay_fresh ? C_DANGER : (flow_on ? 0x7EF082u : C_ORANGE)), 0);

    int pulse_sz = flow_on ? ((rem % 2) ? 36 : 24) : 24;
    lv_obj_set_size(g_act_pulse, pulse_sz, pulse_sz);
    lv_obj_set_pos(g_act_pulse, 460 - pulse_sz / 2, 35 - pulse_sz / 2);
    lv_obj_set_style_bg_color(g_act_pulse,
        lv_color_hex(flow_on ? 0x7EF082u : C_ORANGE), 0);
    lv_obj_set_style_bg_opa(g_act_pulse, flow_on ? LV_OPA_COVER : LV_OPA_50, 0);

    int pct = total > 0 ? (done * 100 / total) : 0;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_bar_set_value(g_act_bar, pct, LV_ANIM_OFF);
}

static void update_controls() {
    bool show  = g_state.controls_visible;
    bool active = g_state.current_run.active;
    bool busy = g_pending.type != PENDING_NONE || g_state.action.busy;
    bool stop_in_progress = g_pending.type == PENDING_STOP_ALL ||
        (g_state.action.busy && g_state.action.type == PENDING_STOP_ALL);
    bool run_starting = g_pending.type == PENDING_RUN_ZONE ||
        (g_state.action.busy && g_state.action.type == PENDING_RUN_ZONE);
    bool control_locked = !g_state.demo_mode && !g_state.controls_auth_configured;

    set_disabled(g_act_stop_btn, stop_in_progress || control_locked);
    lv_label_set_text(g_act_stop_lbl, stop_in_progress ? "STOPPING"
        : (control_locked ? "LOCKED" : "STOP"));
    lv_obj_set_style_text_font(g_act_stop_lbl,
        stop_in_progress ? &lv_font_montserrat_20 : &lv_font_montserrat_32, 0);
    set_disabled(g_stop_btn, stop_in_progress || control_locked);
    lv_label_set_text(g_stop_lbl, stop_in_progress ? "STOPPING ALL ZONES"
        : (control_locked ? "CONTROLS LOCKED" : "STOP ALL WATERING"));

    if (active) {
        lv_obj_set_style_bg_opa(g_ctrl_grp, LV_OPA_TRANSP, 0);
        set_hidden(g_hint_lbl, true);
        set_hidden(g_stop_btn, true);
        set_hidden(g_hist_btn, true);
        set_hidden(g_sched_btn, true);
        for (int i = 0; i < 3; i++) set_hidden(g_chips[i].btn, true);
        return;
    }

    /* Background panel */
    if (show) {
        lv_obj_set_style_bg_color(g_ctrl_grp, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_bg_opa(g_ctrl_grp, LV_OPA_90, 0);
    } else {
        lv_obj_set_style_bg_opa(g_ctrl_grp, LV_OPA_TRANSP, 0);
    }

    set_hidden(g_hint_lbl, show);

    /* All interactive items hidden when not showing controls */
    /* Bridge the short interval after RUN is queued but before the hub's
       WebSocket status reports the active run and its emergency-stop scene. */
    set_hidden(g_stop_btn, !show || active || !run_starting);
    set_hidden(g_hist_btn,  !show);
    set_hidden(g_sched_btn, !show);
    set_disabled(g_hist_btn, busy);
    set_disabled(g_sched_btn, busy);
    for (int i = 0; i < 3; i++)
        set_hidden(g_chips[i].btn, !show || active || run_starting);
    for (int i = 0; i < 3; i++)
        set_disabled(g_chips[i].btn, busy || control_locked || status_is_stale());
}

/* ═══════════════════════════════════════════════════════════
   PUBLIC API
═══════════════════════════════════════════════════════════ */
void ui_show_splash() {
    g_splash_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(g_splash_screen, lv_color_hex(0x06182Fu), 0);
    lv_obj_set_style_bg_grad_color(g_splash_screen, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(g_splash_screen, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(g_splash_screen, 0, 0);

    /* Compose the splash from the small reusable assets.  The former single
       raw splash bitmap consumed roughly 300 KB of flash all by itself. */
    lv_obj_t *robot = lv_img_create(g_splash_screen);
    lv_img_set_src(robot, &img_robot_48);
    lv_img_set_zoom(robot, 512);
    lv_obj_align(robot, LV_ALIGN_CENTER, 0, -92);

    lv_obj_t *title = lv_img_create(g_splash_screen);
    lv_img_set_src(title, &img_title_header);
    lv_img_set_zoom(title, 360);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 4);

    /* "Boise State Edition" subtitle */
    lv_obj_t *sub = lv_label_create(g_splash_screen);
    lv_label_set_text(sub, "BOISE STATE EDITION");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_text_letter_space(sub, 3, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 82);

    g_splash_status = lv_label_create(g_splash_screen);
    lv_label_set_text(g_splash_status, "STARTING...");
    lv_obj_set_style_text_font(g_splash_status, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_splash_status, lv_color_hex(C_MUTED), 0);
    lv_obj_align(g_splash_status, LV_ALIGN_BOTTOM_MID, 0, -24);

    lv_scr_load(g_splash_screen);
}

void ui_set_splash_text(const char *text) {
    if (!g_splash_screen || !g_splash_status) return;
    lv_label_set_text(g_splash_status, text ? text : "");
    lv_timer_handler();
}

void ui_init() { ui_show_splash(); }

void ui_build_dashboard() {
    g_dash_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(g_dash_screen, lv_color_hex(C_BLUE), 0);
    lv_obj_set_style_bg_opa(g_dash_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_dash_screen, 0, 0);
    lv_obj_clear_flag(g_dash_screen, LV_OBJ_FLAG_SCROLLABLE);

    create_background(g_dash_screen);
    create_header(g_dash_screen);
    create_idle_group(g_dash_screen);
    create_active_group(g_dash_screen);
    create_controls_group(g_dash_screen);
    create_duration_picker(g_dash_screen);
    create_toast(g_dash_screen);

    update_background();
    update_header();
    update_idle_group();
    update_active_group();
    update_controls();

    lv_scr_load_anim(g_dash_screen, LV_SCR_LOAD_ANIM_FADE_IN, 300, 60, true);
}

void ui_return_to_dash() {
    g_state.active_screen = 0;
    if (g_dash_screen)
        lv_scr_load_anim(g_dash_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
}

void ui_show_toast(const char *text, uint32_t ms) {
    if (!g_toast || !g_toast_lbl) return;
    lv_label_set_text(g_toast_lbl, text ? text : "");
    lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_toast);
    g_toast_hide_at = millis() + ms;
}

void ui_set_snap_busy(bool busy) {
    (void)busy;
}

static void show_new_control_result() {
    static uint32_t last_result_ms = 0;
    const ActionStatus &action = g_state.action;
    if (action.completed_ms == 0 || action.completed_ms == last_result_ms ||
        (action.type != PENDING_RUN_ZONE && action.type != PENDING_STOP_ALL))
        return;

    last_result_ms = action.completed_ms;
    ui_show_toast(action.message[0] ? action.message
                                    : (action.success ? "CONTROL COMMAND COMPLETE"
                                                      : "CONTROL COMMAND FAILED"),
                  action.success ? 3000 : 5000);
}

void ui_update_timer_cb(lv_timer_t * /*t*/) {
    show_new_control_result();
    if (!g_dash_screen || lv_scr_act() != g_dash_screen) {
        /* Refresh secondary screens if they are active */
        if (g_state.active_screen == 1) ui_schedule_refresh();
        if (g_state.active_screen == 2) ui_history_refresh();
        return;
    }

    static AppState prev = {};
    static bool has_prev = false;
    static PendingType prev_pending = PENDING_NONE;
    static bool prev_status_stale = true;
    static bool prev_weather_stale = true;
    static bool prev_irrigation_stale = true;
    static uint32_t prev_age_bucket = UINT32_MAX;

    bool current_status_stale = status_is_stale();
    bool current_weather_stale = weather_is_stale();
    bool current_irrigation_stale = irrigation_is_stale();
    uint32_t age_bucket = millis() / 5000UL;
    bool age_tick = age_bucket != prev_age_bucket;

    bool bg_chg = !has_prev
        || parse_hour(prev.time_str) != parse_hour(g_state.time_str)
        || memcmp(&prev.weather, &g_state.weather, sizeof(WeatherData)) != 0
        || prev.current_run.active != g_state.current_run.active;

    bool hdr_chg = !has_prev
        || prev.demo_mode     != g_state.demo_mode
        || prev.wifi_connected != g_state.wifi_connected
        || prev.ws_connected   != g_state.ws_connected
        || prev.controls_auth_configured != g_state.controls_auth_configured
        || prev.controls_authenticated != g_state.controls_authenticated
        || current_status_stale != prev_status_stale;

    bool idle_chg = !has_prev
        || minute_changed(prev.time_str, g_state.time_str)
        || strcmp(prev.date_str, g_state.date_str) != 0
        || memcmp(&prev.weather,   &g_state.weather,   sizeof(WeatherData)) != 0
        || memcmp(&prev.irrigation, &g_state.irrigation, sizeof(IrrigationSensorData)) != 0
        || memcmp(&prev.next_run,  &g_state.next_run,  sizeof(NextRun))     != 0
        || prev.current_run.active != g_state.current_run.active
        || current_status_stale != prev_status_stale
        || current_weather_stale != prev_weather_stale
        || current_irrigation_stale != prev_irrigation_stale
        || age_tick;

    bool act_chg = !has_prev
        || memcmp(&prev.current_run, &g_state.current_run, sizeof(CurrentRun)) != 0
        || memcmp(&prev.irrigation, &g_state.irrigation, sizeof(IrrigationSensorData)) != 0
        || (g_state.current_run.active && strcmp(prev.time_str, g_state.time_str) != 0)
        || current_status_stale != prev_status_stale
        || current_irrigation_stale != prev_irrigation_stale;

    bool ctrl_chg = !has_prev
        || prev.controls_visible   != g_state.controls_visible
        || prev.current_run.active != g_state.current_run.active
        || prev.controls_auth_configured != g_state.controls_auth_configured
        || prev.controls_authenticated != g_state.controls_authenticated
        || prev.action.busy != g_state.action.busy
        || prev.action.type != g_state.action.type
        || prev_pending != g_pending.type
        || current_status_stale != prev_status_stale;

    if (bg_chg)   update_background();
    if (hdr_chg)  update_header();
    if (idle_chg) update_idle_group();
    if (act_chg)  update_active_group();
    if (ctrl_chg) update_controls();

    if (g_toast && !(lv_obj_has_flag(g_toast, LV_OBJ_FLAG_HIDDEN)) &&
        g_toast_hide_at && millis() >= g_toast_hide_at) {
        lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
        g_toast_hide_at = 0;
    }

    prev     = g_state;
    has_prev = true;
    prev_pending = g_pending.type;
    prev_status_stale = current_status_stale;
    prev_weather_stale = current_weather_stale;
    prev_irrigation_stale = current_irrigation_stale;
    prev_age_bucket = age_bucket;
}
