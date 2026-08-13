#pragma once
/**
 * ui.h - Public API of the UI layer (ui.cpp, ui_schedule.cpp, ui_history.cpp).
 *
 * Everything else in those files is internal (static). main.cpp and the
 * OTA/screenshot servers should only ever call what is declared here.
 */

#include <lvgl.h>
#include <stdint.h>

/* Boot / splash (ui.cpp) */
void ui_init();
void ui_set_splash_text(const char *text);

/* Main dashboard (ui.cpp) */
void ui_build_dashboard();
void ui_update_timer_cb(lv_timer_t *t);
void ui_return_to_dash();

/* Toast overlay on the dashboard (ui.cpp) */
void ui_show_toast(const char *text, uint32_t ms);

/* Secondary screens */
void ui_schedule_build();    /* ui_schedule.cpp */
void ui_schedule_refresh();
void ui_history_build();     /* ui_history.cpp */
void ui_history_refresh();
