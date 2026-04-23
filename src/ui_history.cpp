/**
 * ui_history.cpp — Scrollable watering history screen
 *
 * Displays the last HIST_MAX runs fetched from GET /api/history
 * (or simulated in demo mode).
 */

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "ui_theme.h"
#include "lawnbot_images.h"

void ui_return_to_dash();

/* ── Screen objects ─────────────────────────────────────── */
static lv_obj_t *g_hist_scr  = nullptr;
static lv_obj_t *g_hist_list = nullptr;   /* lv_list or scroll container */
static lv_obj_t *g_loading_lbl;

/* ── Helpers ─────────────────────────────────────────────── */

/* Parse "YYYY-MM-DDTHH:MM:SS" → "Mar 21  06:00" */
static void fmt_datetime(const char *iso, char *out, size_t n) {
    static const char *MONTHS[13] = {
        "","Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    int yr, mo, dy, hh, mm, ss;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &yr, &mo, &dy, &hh, &mm, &ss) == 6) {
        const char *mon = (mo >= 1 && mo <= 12) ? MONTHS[mo] : "?";
        snprintf(out, n, "%s %02d  %02d:%02d", mon, dy, hh, mm);
    } else {
        snprintf(out, n, "%s", iso);
    }
}

static void fmt_duration(int sec, char *out, size_t n) {
    if (sec < 60)       snprintf(out, n, "%d sec",        sec);
    else if (sec < 3600) snprintf(out, n, "%d min",  sec / 60);
    else                snprintf(out, n, "%dh %dm", sec/3600, (sec%3600)/60);
}

static void rebuild_list() {
    if (!g_hist_list) return;
    lv_obj_clean(g_hist_list);

    if (!g_state.history.valid || g_state.history.count == 0) {
        lv_obj_t *lbl = lv_label_create(g_hist_list);
        lv_label_set_text(lbl, g_state.data_loading
            ? "Loading history..." : "No watering history recorded.");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_MUTED), 0);
        lv_obj_set_width(lbl, 700);
        return;
    }

    for (int i = 0; i < g_state.history.count; i++) {
        const HistEntry &e = g_state.history.entries[i];

        /* Row container */
        lv_obj_t *row = lv_obj_create(g_hist_list);
        lv_obj_set_size(row, 760, 58);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), 0);
        lv_obj_set_style_bg_opa(row, i % 2 == 0 ? LV_OPA_60 : LV_OPA_40, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(C_PANEL_EDGE), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_left(row, 12, 0);
        lv_obj_set_style_pad_right(row, 12, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        /* Zone name */
        lv_obj_t *zone_lbl = lv_label_create(row);
        lv_label_set_text(zone_lbl, zone_display_name(e.zone));
        lv_obj_set_style_text_font(zone_lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(zone_lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_set_width(zone_lbl, 200);
        lv_obj_align(zone_lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* Date/time */
        char dt[24] = "";
        fmt_datetime(e.start_iso, dt, sizeof(dt));
        lv_obj_t *dt_lbl = lv_label_create(row);
        lv_label_set_text(dt_lbl, dt);
        lv_obj_set_style_text_font(dt_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(dt_lbl, lv_color_hex(C_MUTED), 0);
        lv_obj_align(dt_lbl, LV_ALIGN_LEFT_MID, 210, 0);

        /* Duration */
        char dur[16] = "";
        fmt_duration(e.duration_sec, dur, sizeof(dur));
        lv_obj_t *dur_lbl = lv_label_create(row);
        lv_label_set_text(dur_lbl, dur);
        lv_obj_set_style_text_font(dur_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(dur_lbl, lv_color_hex(C_TEXT), 0);
        lv_obj_align(dur_lbl, LV_ALIGN_LEFT_MID, 430, 0);

        /* Manual / scheduled badge */
        lv_obj_t *badge = lv_label_create(row);
        lv_label_set_text(badge, e.is_manual ? "MANUAL" : "SCHED");
        lv_obj_set_style_text_font(badge, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(badge,
            lv_color_hex(e.is_manual ? C_ORANGE : 0x7EF082u), 0);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);

        /* Strike-through style if not completed */
        if (!e.completed) {
            lv_obj_set_style_text_color(zone_lbl, lv_color_hex(C_MUTED), 0);
            lv_obj_t *nc = lv_label_create(row);
            lv_label_set_text(nc, "STOPPED");
            lv_obj_set_style_text_font(nc, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(nc, lv_color_hex(C_DANGER), 0);
            lv_obj_align(nc, LV_ALIGN_RIGHT_MID, -64, 0);
        }
    }
}

static void on_back_tap(lv_event_t * /*e*/) { ui_return_to_dash(); }

/* ── Build ─────────────────────────────────────────────── */
void ui_history_build() {
    if (g_hist_scr) {
        lv_obj_del(g_hist_scr);
        g_hist_scr = nullptr;
    }

    g_hist_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(g_hist_scr, lv_color_hex(C_IDLE_MASK), 0);
    lv_obj_set_style_bg_grad_color(g_hist_scr, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(g_hist_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(g_hist_scr, 0, 0);
    lv_obj_clear_flag(g_hist_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Back button */
    lv_obj_t *back = lv_btn_create(g_hist_scr);
    lv_obj_set_size(back, 90, 44);
    lv_obj_set_pos(back, 16, 14);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_radius(back, 12, 0);
    lv_obj_add_event_cb(back, on_back_tap, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, "< BACK");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(C_MUTED), 0);
    lv_obj_center(bl);

    /* Title */
    lv_obj_t *title = lv_label_create(g_hist_scr);
    lv_label_set_text(title, "WATERING HISTORY");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    /* Robot icon next to title */
    lv_obj_t *robot = lv_img_create(g_hist_scr);
    lv_img_set_src(robot, &img_robot_32);
    lv_obj_align(robot, LV_ALIGN_TOP_MID, -140, 12);

    /* Column headers */
    const char *HDRS[] = {"ZONE", "DATE / TIME", "DURATION", "TYPE"};
    const int   HDR_X[] = {20, 230, 440, 650};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *h = lv_label_create(g_hist_scr);
        lv_label_set_text(h, HDRS[i]);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(h, lv_color_hex(C_MUTED), 0);
        lv_obj_set_pos(h, HDR_X[i], 62);
    }

    /* Separator line */
    lv_obj_t *sep = lv_obj_create(g_hist_scr);
    lv_obj_set_size(sep, 760, 2);
    lv_obj_set_pos(sep, 20, 80);
    lv_obj_set_style_bg_color(sep, lv_color_hex(C_PANEL_EDGE), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    /* Scrollable list container */
    g_hist_list = lv_obj_create(g_hist_scr);
    lv_obj_set_size(g_hist_list, 780, SCREEN_H - 88);
    lv_obj_set_pos(g_hist_list, 10, 86);
    lv_obj_set_style_bg_opa(g_hist_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_hist_list, 0, 0);
    lv_obj_set_style_pad_row(g_hist_list, 6, 0);
    lv_obj_set_style_pad_all(g_hist_list, 0, 0);
    lv_obj_set_flex_flow(g_hist_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(g_hist_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_hist_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_hist_list, LV_SCROLLBAR_MODE_AUTO);

    rebuild_list();

    lv_scr_load_anim(g_hist_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

/* ── Refresh ─────────────────────────────────────────────── */
void ui_history_refresh() {
    if (!g_hist_scr || lv_scr_act() != g_hist_scr) return;
    rebuild_list();
}
