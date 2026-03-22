/**
 * ui_schedule.cpp — 14-day schedule grid + start-times view
 *
 * Tap a day to toggle it on/off.  SAVE button sends PENDING_SAVE_SCHEDULE.
 * In DEMO mode the modified schedule persists in g_state.schedule only.
 */

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "config.h"
#include "ui_theme.h"
#include "lawnbot_images.h"

/* Forward from ui.cpp */
void ui_return_to_dash();

/* ── Screen objects ─────────────────────────────────────── */
static lv_obj_t *g_sched_scr = nullptr;
static lv_obj_t *g_sched_title;
static lv_obj_t *g_save_btn;
static lv_obj_t *g_save_lbl;
static lv_obj_t *g_day_cells[SCHED_DAYS];   /* 14 clickable day tiles */
static lv_obj_t *g_slots_cont;              /* container for start-times text */
static lv_obj_t *g_status_lbl;             /* "Saved!" / "Saving…" feedback */

/* ── Helpers ─────────────────────────────────────────────── */
static const char *DOW_SHORT[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

/* Return day-of-week for a schedule index (today_idx + offset) */
static int dow_for_idx(int sched_idx) {
    /* today's DOW from g_state.date_str ("Saturday  Mar 21") */
    static const char *DAYS[7] = {"Sunday","Monday","Tuesday","Wednesday",
                                   "Thursday","Friday","Saturday"};
    int today_dow = 0;
    for (int i = 0; i < 7; i++) {
        if (strncmp(g_state.date_str, DAYS[i], strlen(DAYS[i])) == 0) {
            today_dow = i; break;
        }
    }
    int offset = sched_idx - g_state.schedule.today_idx;
    return ((today_dow + offset) % 7 + 7) % 7;
}

static void style_day_cell(lv_obj_t *cell, int idx) {
    bool on    = g_state.schedule.days[idx];
    bool today = (idx == g_state.schedule.today_idx);

    lv_obj_set_style_bg_color(cell, lv_color_hex(on ? C_DAY_ON : C_DAY_OFF), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cell,
        lv_color_hex(today ? C_ORANGE : (on ? 0x3A6CC8u : C_PANEL_EDGE)), 0);
    lv_obj_set_style_border_width(cell, today ? 3 : 2, 0);

    /* Update label: DOW short name, then tiny ON/OFF indicator */
    lv_obj_t *dow_lbl = lv_obj_get_child(cell, 0);
    lv_obj_t *ind_lbl = lv_obj_get_child(cell, 1);
    if (dow_lbl) {
        lv_obj_set_style_text_color(dow_lbl,
            lv_color_hex(today ? C_ORANGE : (on ? C_TEXT : C_MUTED)), 0);
    }
    if (ind_lbl) {
        lv_label_set_text(ind_lbl, on ? "ON" : "OFF");
        lv_obj_set_style_text_color(ind_lbl,
            lv_color_hex(on ? 0x7EF082u : 0x556070u), 0);
    }
}

static void refresh_slots_panel() {
    if (!g_slots_cont) return;
    lv_obj_clean(g_slots_cont);

    if (!g_state.schedule.valid || g_state.schedule.num_slots == 0) {
        lv_obj_t *lbl = lv_label_create(g_slots_cont);
        lv_label_set_text(lbl, g_state.data_loading
            ? "Loading schedule..." : "No start times configured.");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_MUTED), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        return;
    }

    int y = 0;
    for (int s = 0; s < g_state.schedule.num_slots; s++) {
        const SchedSlot &slot = g_state.schedule.slots[s];

        /* Time row */
        lv_obj_t *time_lbl = lv_label_create(g_slots_cont);
        char time_buf[32];
        snprintf(time_buf, sizeof(time_buf), "%s %s",
                 slot.enabled ? "" : "(disabled)  ",
                 slot.time_str);
        lv_label_set_text(time_lbl, slot.time_str);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(time_lbl, lv_color_hex(slot.enabled ? C_ORANGE : C_MUTED), 0);
        lv_obj_set_pos(time_lbl, 0, y);
        y += 30;

        /* Zone rows */
        for (int z = 0; z < slot.num_zones; z++) {
            const SchedZone &zone = slot.zones[z];
            lv_obj_t *zlbl = lv_label_create(g_slots_cont);
            char zbuf[64];
            snprintf(zbuf, sizeof(zbuf), "   %s  —  %.0f min",
                     zone_display_name(zone.name), zone.duration_min);
            lv_label_set_text(zlbl, zbuf);
            lv_obj_set_style_text_font(zlbl, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(zlbl,
                lv_color_hex(zone.enabled ? C_TEXT : C_MUTED), 0);
            lv_obj_set_pos(zlbl, 0, y);
            y += 24;
        }
        y += 8; /* gap between slots */
    }
}

/* ── Day cell tap callback ──────────────────────────────── */
static void on_day_tap(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= SCHED_DAYS) return;
    g_state.schedule.days[idx] = !g_state.schedule.days[idx];
    g_state.schedule.days_dirty = true;
    style_day_cell(g_day_cells[idx], idx);

    /* Show SAVE button */
    lv_obj_clear_flag(g_save_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_status_lbl, "");
}

static void on_save_tap(lv_event_t * /*e*/) {
    lv_label_set_text(g_save_lbl, "SAVING...");
    lv_label_set_text(g_status_lbl, "");
    g_pending.type = PENDING_SAVE_SCHEDULE;
}

static void on_back_tap(lv_event_t * /*e*/) {
    ui_return_to_dash();
}

/* ── Build ─────────────────────────────────────────────── */
void ui_schedule_build() {
    /* Delete previous instance if any */
    if (g_sched_scr) {
        lv_obj_del(g_sched_scr);
        g_sched_scr = nullptr;
    }

    g_sched_scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(g_sched_scr, lv_color_hex(C_IDLE_MASK), 0);
    lv_obj_set_style_bg_grad_color(g_sched_scr, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_grad_dir(g_sched_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(g_sched_scr, 0, 0);
    lv_obj_clear_flag(g_sched_scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Header row ───────────────────────────────────── */
    lv_obj_t *back_btn = lv_btn_create(g_sched_scr);
    lv_obj_set_size(back_btn, 90, 44);
    lv_obj_set_pos(back_btn, 16, 14);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 12, 0);
    lv_obj_add_event_cb(back_btn, on_back_tap, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *bl = lv_label_create(back_btn);
    lv_label_set_text(bl, "< BACK");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(C_MUTED), 0);
    lv_obj_center(bl);

    g_sched_title = lv_label_create(g_sched_scr);
    lv_label_set_text(g_sched_title, "WATERING SCHEDULE");
    lv_obj_set_style_text_font(g_sched_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_sched_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_letter_space(g_sched_title, 2, 0);
    lv_obj_align(g_sched_title, LV_ALIGN_TOP_MID, 0, 20);

    /* Robot icon next to title */
    lv_obj_t *robot = lv_img_create(g_sched_scr);
    lv_img_set_src(robot, &img_robot_32);
    lv_obj_align(robot, LV_ALIGN_TOP_MID, -148, 12);

    g_save_btn = lv_btn_create(g_sched_scr);
    lv_obj_set_size(g_save_btn, 100, 44);
    lv_obj_set_pos(g_save_btn, 686, 14);
    lv_obj_set_style_bg_color(g_save_btn, lv_color_hex(C_SUCCESS), 0);
    lv_obj_set_style_border_width(g_save_btn, 0, 0);
    lv_obj_set_style_radius(g_save_btn, 12, 0);
    lv_obj_add_flag(g_save_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_save_btn, on_save_tap, LV_EVENT_CLICKED, nullptr);
    g_save_lbl = lv_label_create(g_save_btn);
    lv_label_set_text(g_save_lbl, "SAVE");
    lv_obj_set_style_text_font(g_save_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_save_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(g_save_lbl);

    g_status_lbl = lv_label_create(g_sched_scr);
    lv_label_set_text(g_status_lbl, "");
    lv_obj_set_style_text_font(g_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_status_lbl, lv_color_hex(C_SUCCESS), 0);
    lv_obj_align(g_status_lbl, LV_ALIGN_TOP_RIGHT, -16, 64);

    /* ── Section label ─────────────────────────────────── */
    lv_obj_t *sec_lbl = lv_label_create(g_sched_scr);
    lv_label_set_text(sec_lbl, "WATERING DAYS  (tap to toggle)");
    lv_obj_set_style_text_font(sec_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sec_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_letter_space(sec_lbl, 1, 0);
    lv_obj_align(sec_lbl, LV_ALIGN_TOP_LEFT, 24, 72);

    /* ── 14-day grid (2 rows × 7) ─────────────────────── */
    /* Cell size: 100×68, gap: 8. Total row: 7×100+6×8=748. Left: (800-748)/2=26 */
    const int CW = 100, CH = 68, GAP = 8;
    const int GRID_X0 = 26, GRID_Y0 = 100;

    for (int i = 0; i < SCHED_DAYS; i++) {
        int col = i % 7, row = i / 7;
        int cx = GRID_X0 + col * (CW + GAP);
        int cy = GRID_Y0 + row * (CH + GAP);

        lv_obj_t *cell = lv_obj_create(g_sched_scr);
        lv_obj_set_size(cell, CW, CH);
        lv_obj_set_pos(cell, cx, cy);
        lv_obj_set_style_radius(cell, 12, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, on_day_tap, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        /* Day-of-week abbreviation */
        lv_obj_t *dow = lv_label_create(cell);
        lv_label_set_text(dow, DOW_SHORT[dow_for_idx(i)]);
        lv_obj_set_style_text_font(dow, &lv_font_montserrat_14, 0);
        lv_obj_align(dow, LV_ALIGN_TOP_MID, 0, 8);

        /* ON/OFF indicator */
        lv_obj_t *ind = lv_label_create(cell);
        lv_label_set_text(ind, "ON");
        lv_obj_set_style_text_font(ind, &lv_font_montserrat_12, 0);
        lv_obj_align(ind, LV_ALIGN_BOTTOM_MID, 0, -8);

        g_day_cells[i] = cell;
        style_day_cell(cell, i);
    }

    /* Legend */
    lv_obj_t *leg = lv_label_create(g_sched_scr);
    lv_label_set_text(leg, "BLUE = watering day   DARK = skip   ORANGE border = today");
    lv_obj_set_style_text_font(leg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(leg, lv_color_hex(C_MUTED), 0);
    lv_obj_align(leg, LV_ALIGN_TOP_MID, 0, GRID_Y0 + 2*(CH+GAP) + 6);

    /* ── Start times section ─────────────────────────── */
    int slots_y = GRID_Y0 + 2*(CH+GAP) + 30;
    lv_obj_t *st_lbl = lv_label_create(g_sched_scr);
    lv_label_set_text(st_lbl, "START TIMES");
    lv_obj_set_style_text_font(st_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_letter_space(st_lbl, 1, 0);
    lv_obj_set_pos(st_lbl, 24, slots_y);

    /* Scrollable slots panel */
    g_slots_cont = lv_obj_create(g_sched_scr);
    lv_obj_set_size(g_slots_cont, 760, SCREEN_H - slots_y - 28);
    lv_obj_set_pos(g_slots_cont, 20, slots_y + 26);
    lv_obj_set_style_bg_opa(g_slots_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_slots_cont, 0, 0);
    lv_obj_set_style_pad_all(g_slots_cont, 0, 0);
    lv_obj_set_flex_flow(g_slots_cont, LV_FLEX_FLOW_COLUMN);

    refresh_slots_panel();

    lv_scr_load_anim(g_sched_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

/* ── Refresh (called from ui_update_timer_cb) ─────────── */
void ui_schedule_refresh() {
    if (!g_sched_scr || lv_scr_act() != g_sched_scr) return;

    /* If save just completed, show feedback and hide SAVE button */
    if (g_pending.type == PENDING_NONE && g_state.schedule.days_dirty == false) {
        lv_label_set_text(g_save_lbl, "SAVE");
        /* Show "Saved!" momentarily */
    }

    refresh_slots_panel();
}
