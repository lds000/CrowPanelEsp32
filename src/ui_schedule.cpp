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
static lv_obj_t *g_add_btn;
static lv_obj_t *g_day_cells[SCHED_DAYS];   /* 14 clickable day tiles */
static lv_obj_t *g_slots_cont;              /* container for start-times text */
static lv_obj_t *g_time_btns[SCHED_MAX_SLOTS] = {};
static lv_obj_t *g_time_picker = nullptr;
static lv_obj_t *g_time_title = nullptr;
static lv_obj_t *g_time_value = nullptr;
static int g_edit_slot = -1;
static int g_edit_hour = 0;
static int g_edit_min = 0;
static uint32_t g_last_view_signature = 0;
static bool g_view_signature_valid = false;
static bool g_save_requested = false;
static uint32_t g_save_started_ms = 0;
static uint32_t g_last_save_result_ms = 0;
static uint32_t g_status_hold_until_ms = 0;
static lv_obj_t *g_status_lbl;             /* "Saved!" / "Saving…" feedback */

/* ── Helpers ─────────────────────────────────────────────── */
static const char *DOW_SHORT[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};

static void on_time_tap(lv_event_t *e);
static void on_time_adjust(lv_event_t *e);
static void on_time_ok(lv_event_t *e);
static void on_time_cancel(lv_event_t *e);
static void on_add_time_tap(lv_event_t *e);

static void set_disabled(lv_obj_t *obj, bool disabled) {
    if (!obj) return;
    if (disabled) lv_obj_add_state(obj, LV_STATE_DISABLED);
    else          lv_obj_clear_state(obj, LV_STATE_DISABLED);
}

static bool schedule_control_ready() {
    if (g_state.demo_mode) return true;
    bool fresh = g_state.ws_connected && g_state.status_updated_ms != 0 &&
        (uint32_t)(millis() - g_state.status_updated_ms) <= UI_STATUS_STALE_MS;
    return fresh && g_state.controls_auth_configured &&
           g_state.schedule.valid && g_state.schedule.save_supported;
}

static const char *schedule_read_only_reason() {
    if (!g_state.schedule.save_supported) return "READ ONLY: LOSSLESS SAVE UNAVAILABLE";
    if (!g_state.demo_mode && !g_state.controls_auth_configured)
        return "READ ONLY: CONTROL TOKEN MISSING";
    if (!g_state.demo_mode && (!g_state.ws_connected ||
        g_state.status_updated_ms == 0 ||
        (uint32_t)(millis() - g_state.status_updated_ms) > UI_STATUS_STALE_MS))
        return "READ ONLY: HUB DATA STALE";
    return "SCHEDULE IS READ ONLY";
}

static void show_status(const char *text, uint32_t color) {
    lv_label_set_text(g_status_lbl, text ? text : "");
    lv_obj_set_style_text_color(g_status_lbl, lv_color_hex(color), 0);
}

static void show_read_only_status() {
    show_status(schedule_read_only_reason(), C_ORANGE);
}

static uint32_t fnv1a(const void *data, size_t len, uint32_t hash) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t schedule_view_signature() {
    uint32_t hash = fnv1a(&g_state.schedule, sizeof(g_state.schedule), 2166136261u);
    hash = fnv1a(&g_state.data_loading, sizeof(g_state.data_loading), hash);
    bool editable = schedule_control_ready();
    return fnv1a(&editable, sizeof(editable), hash);
}

static void mark_schedule_dirty() {
    if (!schedule_control_ready()) {
        show_read_only_status();
        return;
    }
    g_state.schedule.days_dirty = true;
    lv_obj_clear_flag(g_save_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(g_save_lbl, "SAVE");
    show_status("UNSAVED CHANGES", C_ORANGE);
}

static void parse_time(const char *text, int *hour, int *minute) {
    int h = 0, m = 0;
    if (!text || sscanf(text, "%d:%d", &h, &m) != 2) {
        h = 6;
        m = 0;
    }
    *hour = ((h % 24) + 24) % 24;
    *minute = ((m % 60) + 60) % 60;
}

static void update_time_picker_value() {
    if (!g_time_value) return;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", g_edit_hour, g_edit_min);
    lv_label_set_text(g_time_value, buf);
}

static void format_time_from_minutes(int total, char *out, size_t n) {
    total = ((total % (24 * 60)) + (24 * 60)) % (24 * 60);
    snprintf(out, n, "%02d:%02d", total / 60, total % 60);
}

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
    int32_t scroll_y = lv_obj_get_scroll_y(g_slots_cont);
    lv_obj_clean(g_slots_cont);
    memset(g_time_btns, 0, sizeof(g_time_btns));

    if (!g_state.schedule.valid || g_state.schedule.num_slots == 0) {
        lv_obj_t *lbl = lv_label_create(g_slots_cont);
        lv_label_set_text(lbl, g_state.data_loading
            ? "Loading schedule..." : "No start times configured.");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(C_MUTED), 0);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
        g_last_view_signature = schedule_view_signature();
        g_view_signature_valid = true;
        return;
    }

    int y = 0;
    for (int s = 0; s < g_state.schedule.num_slots; s++) {
        const SchedSlot &slot = g_state.schedule.slots[s];

        /* Time row */
        lv_obj_t *time_btn = lv_btn_create(g_slots_cont);
        g_time_btns[s] = time_btn;
        lv_obj_set_size(time_btn, 146, 42);
        lv_obj_set_pos(time_btn, 0, y);
        lv_obj_set_style_bg_color(time_btn, lv_color_hex(0x0A2240u), 0);
        lv_obj_set_style_bg_opa(time_btn, LV_OPA_80, 0);
        lv_obj_set_style_border_color(time_btn,
            lv_color_hex(slot.enabled ? C_ORANGE : C_PANEL_EDGE), 0);
        lv_obj_set_style_border_width(time_btn, 2, 0);
        lv_obj_set_style_radius(time_btn, 12, 0);
        lv_obj_add_event_cb(time_btn, on_time_tap, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(s)));
        set_disabled(time_btn, !schedule_control_ready() || g_state.action.busy ||
                                g_pending.type != PENDING_NONE);

        lv_obj_t *time_lbl = lv_label_create(time_btn);
        char time_buf[32];
        snprintf(time_buf, sizeof(time_buf), "%s%s",
                 slot.enabled ? "" : "OFF ",
                 slot.time_str);
        lv_label_set_text(time_lbl, time_buf);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(time_lbl, lv_color_hex(slot.enabled ? C_ORANGE : C_MUTED), 0);
        lv_obj_center(time_lbl);
        y += 46;

        /* Zone rows */
        for (int z = 0; z < slot.num_zones; z++) {
            const SchedZone &zone = slot.zones[z];
            lv_obj_t *zlbl = lv_label_create(g_slots_cont);
            char zbuf[64];
            snprintf(zbuf, sizeof(zbuf), "   %s  -  %.0f min",
                     zone_display_name(zone.name), zone.duration_min);
            lv_label_set_text(zlbl, zbuf);
            lv_obj_set_style_text_font(zlbl, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(zlbl,
                lv_color_hex(zone.enabled ? C_TEXT : C_MUTED), 0);
            lv_obj_set_pos(zlbl, 0, y);
            y += 24;
        }
        y += 8; /* gap between slots */
    }

    lv_obj_update_layout(g_slots_cont);
    lv_obj_scroll_to_y(g_slots_cont, scroll_y, LV_ANIM_OFF);
    g_last_view_signature = schedule_view_signature();
    g_view_signature_valid = true;
}

/* ── Day cell tap callback ──────────────────────────────── */
static void on_day_tap(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= SCHED_DAYS) return;
    if (!schedule_control_ready() || g_state.action.busy ||
        g_pending.type != PENDING_NONE) {
        show_read_only_status();
        return;
    }
    g_state.schedule.days[idx] = !g_state.schedule.days[idx];
    mark_schedule_dirty();
    style_day_cell(g_day_cells[idx], idx);
}

static void on_save_tap(lv_event_t * /*e*/) {
    if (!g_state.schedule.days_dirty) return;
    if (!schedule_control_ready() || g_state.action.busy ||
        g_pending.type != PENDING_NONE) {
        show_read_only_status();
        return;
    }
    lv_label_set_text(g_save_lbl, "SAVING...");
    show_status("SAVING SCHEDULE...", C_ORANGE);
    set_disabled(g_save_btn, true);
    g_save_requested = true;
    g_save_started_ms = millis();
    g_pending.type = PENDING_SAVE_SCHEDULE;
}

static void on_back_tap(lv_event_t * /*e*/) {
    if (g_state.action.busy || g_pending.type != PENDING_NONE) {
        show_status("PLEASE WAIT FOR CURRENT REQUEST", C_ORANGE);
        return;
    }
    ui_return_to_dash();
}

/* ── Build ─────────────────────────────────────────────── */
static void on_add_time_tap(lv_event_t * /*e*/) {
    if (!schedule_control_ready() || g_state.action.busy ||
        g_pending.type != PENDING_NONE) {
        show_read_only_status();
        return;
    }
    if (g_state.schedule.num_slots >= SCHED_MAX_SLOTS) {
        show_status("MAX 4 START TIMES", C_ORANGE);
        return;
    }
    if (g_state.schedule.truncated) {
        show_status("ADD DISABLED: HIDDEN HUB SETS", C_ORANGE);
        return;
    }

    int idx = g_state.schedule.num_slots;
    SchedSlot &slot = g_state.schedule.slots[idx];
    memset(&slot, 0, sizeof(slot));
    slot.enabled = true;

    if (idx > 0) {
        int h = 0, m = 0;
        parse_time(g_state.schedule.slots[idx - 1].time_str, &h, &m);
        format_time_from_minutes(h * 60 + m + 60, slot.time_str, sizeof(slot.time_str));
    } else {
        strlcpy(slot.time_str, "06:00", sizeof(slot.time_str));
    }

    if (g_state.schedule.num_slots > 0) {
        const SchedSlot &template_slot = g_state.schedule.slots[0];
        slot.num_zones = template_slot.num_zones;
        for (int z = 0; z < template_slot.num_zones && z < 3; z++) {
            slot.zones[z] = template_slot.zones[z];
        }
    } else {
        slot.num_zones = 3;
        for (int z = 0; z < 3; z++) {
            strlcpy(slot.zones[z].name, ZONE_API_NAMES[z], sizeof(slot.zones[z].name));
            slot.zones[z].duration_min = DEFAULT_RUN_MINUTES;
            slot.zones[z].enabled = true;
        }
    }

    g_state.schedule.num_slots++;
    mark_schedule_dirty();
    refresh_slots_panel();
}

static void on_time_tap(lv_event_t *e) {
    int slot_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (slot_idx < 0 || slot_idx >= g_state.schedule.num_slots) return;
    if (!schedule_control_ready() || g_state.action.busy ||
        g_pending.type != PENDING_NONE) {
        show_read_only_status();
        return;
    }

    g_edit_slot = slot_idx;
    parse_time(g_state.schedule.slots[slot_idx].time_str, &g_edit_hour, &g_edit_min);

    char title[32];
    snprintf(title, sizeof(title), "EDIT START TIME %d", slot_idx + 1);
    lv_label_set_text(g_time_title, title);
    update_time_picker_value();
    lv_obj_clear_flag(g_time_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_time_picker);
}

static void on_time_adjust(lv_event_t *e) {
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int total = g_edit_hour * 60 + g_edit_min + delta;
    total = ((total % (24 * 60)) + (24 * 60)) % (24 * 60);
    g_edit_hour = total / 60;
    g_edit_min = total % 60;
    update_time_picker_value();
}

static void on_time_ok(lv_event_t * /*e*/) {
    if (g_edit_slot >= 0 && g_edit_slot < g_state.schedule.num_slots &&
        schedule_control_ready() && !g_state.action.busy &&
        g_pending.type == PENDING_NONE) {
        snprintf(g_state.schedule.slots[g_edit_slot].time_str,
                 sizeof(g_state.schedule.slots[g_edit_slot].time_str),
                 "%02d:%02d", g_edit_hour, g_edit_min);
        mark_schedule_dirty();
        refresh_slots_panel();
    } else if (g_edit_slot >= 0) {
        show_read_only_status();
    }
    lv_obj_add_flag(g_time_picker, LV_OBJ_FLAG_HIDDEN);
    g_edit_slot = -1;
}

static void on_time_cancel(lv_event_t * /*e*/) {
    lv_obj_add_flag(g_time_picker, LV_OBJ_FLAG_HIDDEN);
    g_edit_slot = -1;
}

static lv_obj_t *make_time_picker_button(lv_obj_t *parent, int x, int y,
                                         const char *text, int delta) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 116, 52);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_BLUE), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_ORANGE), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_add_event_cb(btn, on_time_adjust, LV_EVENT_CLICKED,
                        reinterpret_cast<void *>(static_cast<intptr_t>(delta)));

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(lbl);
    return btn;
}

static void create_time_picker(lv_obj_t *scr) {
    g_time_picker = lv_obj_create(scr);
    lv_obj_set_size(g_time_picker, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_time_picker, 0, 0);
    lv_obj_set_style_bg_color(g_time_picker, lv_color_hex(0x000000u), 0);
    lv_obj_set_style_bg_opa(g_time_picker, LV_OPA_70, 0);
    lv_obj_set_style_border_width(g_time_picker, 0, 0);
    lv_obj_set_style_radius(g_time_picker, 0, 0);
    lv_obj_clear_flag(g_time_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_time_picker, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(g_time_picker);
    lv_obj_set_size(card, 560, 330);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    g_time_title = lv_label_create(card);
    lv_label_set_text(g_time_title, "EDIT START TIME");
    lv_obj_set_width(g_time_title, 520);
    lv_obj_set_style_text_align(g_time_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_time_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_time_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_pos(g_time_title, 20, 18);

    g_time_value = lv_label_create(card);
    lv_label_set_text(g_time_value, "06:00");
    lv_obj_set_width(g_time_value, 520);
    lv_obj_set_style_text_align(g_time_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(g_time_value, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(g_time_value, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_pos(g_time_value, 20, 70);

    make_time_picker_button(card,  40, 142, "H -", -60);
    make_time_picker_button(card, 168, 142, "H +",  60);
    make_time_picker_button(card, 296, 142, "M -",  -5);
    make_time_picker_button(card, 424, 142, "M +",   5);

    lv_obj_t *cancel = lv_btn_create(card);
    lv_obj_set_size(cancel, 160, 58);
    lv_obj_set_pos(cancel, 100, 242);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x1A2840u), 0);
    lv_obj_set_style_border_color(cancel, lv_color_hex(C_PANEL_EDGE), 0);
    lv_obj_set_style_border_width(cancel, 2, 0);
    lv_obj_set_style_radius(cancel, 14, 0);
    lv_obj_add_event_cb(cancel, on_time_cancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(cancel);
    lv_label_set_text(cl, "CANCEL");
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cl, lv_color_hex(C_MUTED), 0);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_btn_create(card);
    lv_obj_set_size(ok, 160, 58);
    lv_obj_set_pos(ok, 300, 242);
    lv_obj_set_style_bg_color(ok, lv_color_hex(C_SUCCESS), 0);
    lv_obj_set_style_border_width(ok, 0, 0);
    lv_obj_set_style_radius(ok, 14, 0);
    lv_obj_add_event_cb(ok, on_time_ok, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *ol = lv_label_create(ok);
    lv_label_set_text(ol, "OK");
    lv_obj_set_style_text_font(ol, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(ol, lv_color_hex(C_TEXT), 0);
    lv_obj_center(ol);
}

static bool deadline_active(uint32_t deadline) {
    return deadline != 0 && (int32_t)(deadline - millis()) > 0;
}

static void update_schedule_interactivity() {
    bool request_busy = g_state.action.busy || g_pending.type != PENDING_NONE;
    bool editable = schedule_control_ready() && !request_busy;
    for (int i = 0; i < SCHED_DAYS; ++i) set_disabled(g_day_cells[i], !editable);
    for (int i = 0; i < SCHED_MAX_SLOTS; ++i) set_disabled(g_time_btns[i], !editable);
    set_disabled(g_add_btn, !editable || g_state.schedule.truncated);
    set_disabled(g_save_btn, !editable || !g_state.schedule.days_dirty);
}

static void show_schedule_freshness() {
    if (!g_state.schedule.valid) {
        show_status(g_state.data_loading ? "LOADING SCHEDULE..." : "SCHEDULE UNAVAILABLE",
                    g_state.data_loading ? C_ORANGE : C_DANGER);
        return;
    }
    if (!schedule_control_ready()) {
        show_read_only_status();
        return;
    }
    if (g_state.schedule.days_dirty) {
        show_status("UNSAVED CHANGES", C_ORANGE);
        return;
    }
    if (g_state.schedule.truncated) {
        show_status("HIDDEN HUB SETS PRESERVED", C_ORANGE);
        return;
    }
    if (g_state.schedule.updated_ms != 0) {
        char text[48];
        uint32_t age = (uint32_t)(millis() - g_state.schedule.updated_ms) / 1000UL;
        snprintf(text, sizeof(text), "UPDATED %lus AGO", (unsigned long)age);
        show_status(text, C_MUTED);
    } else {
        show_status("", C_MUTED);
    }
}

static void update_save_feedback() {
    bool saving = (g_pending.type == PENDING_SAVE_SCHEDULE) ||
                  (g_state.action.busy && g_state.action.type == PENDING_SAVE_SCHEDULE);
    if (saving) {
        if (!g_save_started_ms) g_save_started_ms = millis();
        g_save_requested = true;
        lv_label_set_text(g_save_lbl, "SAVING...");
        set_disabled(g_save_btn, true);
        if ((uint32_t)(millis() - g_save_started_ms) > 15000UL)
            show_status("SAVE STILL WORKING - HUB IS SLOW", C_ORANGE);
        else
            show_status(g_state.action.message[0] ? g_state.action.message
                                                  : "SAVING SCHEDULE...", C_ORANGE);
        return;
    }

    if (g_state.action.type == PENDING_SAVE_SCHEDULE &&
        g_state.action.completed_ms != 0 &&
        g_state.action.completed_ms != g_last_save_result_ms) {
        g_last_save_result_ms = g_state.action.completed_ms;
        g_save_requested = false;
        g_save_started_ms = 0;
        g_status_hold_until_ms = millis() + 6000UL;
        lv_label_set_text(g_save_lbl, "SAVE");
        show_status(g_state.action.message[0] ? g_state.action.message
                                             : (g_state.action.success ? "SCHEDULE SAVED"
                                                                       : "SAVE FAILED"),
                    g_state.action.success ? C_SUCCESS : C_DANGER);
        if (g_state.action.success && !g_state.schedule.days_dirty)
            lv_obj_add_flag(g_save_btn, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(g_save_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Demo mode completes synchronously and older backends may not populate
       ActionStatus.  Preserve useful feedback for both. */
    if (g_save_requested && g_pending.type == PENDING_NONE && !g_state.action.busy) {
        bool success = !g_state.schedule.days_dirty;
        g_save_requested = false;
        g_save_started_ms = 0;
        g_status_hold_until_ms = millis() + 6000UL;
        lv_label_set_text(g_save_lbl, "SAVE");
        show_status(success ? "SCHEDULE SAVED" : "SAVE FAILED",
                    success ? C_SUCCESS : C_DANGER);
        if (success) lv_obj_add_flag(g_save_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (!deadline_active(g_status_hold_until_ms)) {
        g_status_hold_until_ms = 0;
        show_schedule_freshness();
    }
}

void ui_schedule_build() {
    /* Delete previous instance if any */
    if (g_sched_scr) {
        lv_obj_del(g_sched_scr);
        g_sched_scr = nullptr;
    }
    g_view_signature_valid = false;
    g_add_btn = nullptr;
    g_status_hold_until_ms = 0;
    g_last_save_result_ms = (g_state.action.type == PENDING_SAVE_SCHEDULE)
        ? g_state.action.completed_ms : 0;

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
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(C_MUTED), 0);
    lv_obj_center(bl);

    g_sched_title = lv_label_create(g_sched_scr);
    lv_label_set_text(g_sched_title, "WATERING SCHEDULE");
    lv_obj_set_style_text_font(g_sched_title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(g_sched_title, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_letter_space(g_sched_title, 2, 0);
    lv_obj_align(g_sched_title, LV_ALIGN_TOP_MID, 0, 20);

    /* Robot icon next to title */
    lv_obj_t *robot = lv_img_create(g_sched_scr);
    lv_img_set_src(robot, &img_robot_32);
    lv_obj_align(robot, LV_ALIGN_TOP_MID, -220, 12);

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
    lv_obj_set_style_text_font(g_save_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_save_lbl, lv_color_hex(C_TEXT), 0);
    lv_obj_center(g_save_lbl);

    g_status_lbl = lv_label_create(g_sched_scr);
    lv_label_set_text(g_status_lbl, "");
    lv_obj_set_style_text_font(g_status_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(g_status_lbl, lv_color_hex(C_SUCCESS), 0);
    lv_obj_align(g_status_lbl, LV_ALIGN_TOP_RIGHT, -16, 64);

    /* ── Section label ─────────────────────────────────── */
    lv_obj_t *sec_lbl = lv_label_create(g_sched_scr);
    lv_label_set_text(sec_lbl, "WATERING DAYS  (tap to toggle)");
    lv_obj_set_style_text_font(sec_lbl, &lv_font_montserrat_18, 0);
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
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, on_day_tap, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        /* Day-of-week abbreviation */
        lv_obj_t *dow = lv_label_create(cell);
        lv_label_set_text(dow, DOW_SHORT[dow_for_idx(i)]);
        lv_obj_set_width(dow, CW);
        lv_obj_set_style_text_align(dow, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(dow, &lv_font_montserrat_18, 0);
        lv_obj_set_pos(dow, 0, 35);

        /* ON/OFF indicator */
        lv_obj_t *ind = lv_label_create(cell);
        lv_label_set_text(ind, "ON");
        lv_obj_set_width(ind, CW);
        lv_obj_set_style_text_align(ind, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(ind, &lv_font_montserrat_16, 0);
        lv_obj_set_pos(ind, 0, 15);

        g_day_cells[i] = cell;
        style_day_cell(cell, i);
    }

    /* Legend */
    lv_obj_t *leg = lv_label_create(g_sched_scr);
    lv_label_set_text(leg, "BLUE = watering day   DARK = skip   ORANGE border = today");
    lv_obj_set_style_text_font(leg, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(leg, lv_color_hex(C_MUTED), 0);
    lv_obj_align(leg, LV_ALIGN_TOP_MID, 0, GRID_Y0 + 2*(CH+GAP) + 6);

    /* ── Start times section ─────────────────────────── */
    int slots_y = GRID_Y0 + 2*(CH+GAP) + 30;
    lv_obj_t *st_lbl = lv_label_create(g_sched_scr);
    lv_label_set_text(st_lbl, "START TIMES  (tap time to edit)");
    lv_obj_set_style_text_font(st_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(st_lbl, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_text_letter_space(st_lbl, 1, 0);
    lv_obj_set_pos(st_lbl, 24, slots_y);

    g_add_btn = lv_btn_create(g_sched_scr);
    lv_obj_set_size(g_add_btn, 116, 38);
    lv_obj_set_pos(g_add_btn, 664, slots_y - 8);
    lv_obj_set_style_bg_color(g_add_btn, lv_color_hex(0x0A2240u), 0);
    lv_obj_set_style_bg_opa(g_add_btn, LV_OPA_90, 0);
    lv_obj_set_style_border_color(g_add_btn, lv_color_hex(C_ORANGE), 0);
    lv_obj_set_style_border_width(g_add_btn, 2, 0);
    lv_obj_set_style_radius(g_add_btn, 12, 0);
    lv_obj_add_event_cb(g_add_btn, on_add_time_tap, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *add_lbl = lv_label_create(g_add_btn);
    lv_label_set_text(add_lbl, "+ TIME");
    lv_obj_set_style_text_font(add_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(add_lbl, lv_color_hex(C_ORANGE), 0);
    lv_obj_center(add_lbl);

    /* Scrollable slots panel */
    g_slots_cont = lv_obj_create(g_sched_scr);
    lv_obj_set_size(g_slots_cont, 760, SCREEN_H - slots_y - 28);
    lv_obj_set_pos(g_slots_cont, 20, slots_y + 26);
    lv_obj_set_style_bg_opa(g_slots_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_slots_cont, 0, 0);
    lv_obj_set_style_pad_all(g_slots_cont, 0, 0);
    lv_obj_set_flex_flow(g_slots_cont, LV_FLEX_FLOW_COLUMN);

    refresh_slots_panel();
    create_time_picker(g_sched_scr);
    update_schedule_interactivity();
    if (g_state.schedule.days_dirty && schedule_control_ready())
        lv_obj_clear_flag(g_save_btn, LV_OBJ_FLAG_HIDDEN);
    update_save_feedback();

    lv_scr_load_anim(g_sched_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

/* ── Refresh (called from ui_update_timer_cb) ─────────── */
void ui_schedule_refresh() {
    if (!g_sched_scr || lv_scr_act() != g_sched_scr) return;

    uint32_t signature = schedule_view_signature();
    if (!g_view_signature_valid || signature != g_last_view_signature) {
        for (int i = 0; i < SCHED_DAYS; ++i) style_day_cell(g_day_cells[i], i);
        refresh_slots_panel();
    }
    update_schedule_interactivity();
    update_save_feedback();
}
