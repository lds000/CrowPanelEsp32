#pragma once
#include <Arduino.h>

/**
 * Shared application state — written by WiFi/WebSocket/HTTP tasks,
 * read by the LVGL UI update timer.
 */

/* ── Zone relay state ────────────────────────────────────── */
struct ZoneRelays {
    bool hanging_pots;
    bool garden;
    bool misters;
};

/* ── Active run (null-pattern: active==false means no run) ── */
struct CurrentRun {
    bool active;
    char zone[32];        /* API name, e.g. "Garden" */
    int  remaining_sec;
    int  total_sec;
    bool is_manual;
};

/* ── Next scheduled run ──────────────────────────────────── */
struct NextRun {
    bool valid;
    char zone[32];        /* API name */
    char time_str[8];     /* "HH:MM" */
    char date_str[12];    /* "YYYY-MM-DD" */
};

/* ── Weather / sensor data ────────────────────────────────── */
struct WeatherData {
    bool  valid;
    float temp_f;
    float humidity_pct;
    float wind_mph;
    char  wind_dir[4];
};

/* ── Schedule data ─────────────────────────────────────────── */
#define SCHED_DAYS       14
#define SCHED_MAX_SLOTS   4

struct SchedZone {
    char  name[32];       /* e.g. "Garden" */
    float duration_min;
    bool  enabled;
};

struct SchedSlot {
    char    time_str[8];  /* "HH:MM" */
    bool    enabled;
    SchedZone zones[3];
    int     num_zones;
};

struct ScheduleData {
    bool      valid;
    bool      days[SCHED_DAYS];    /* which of the 14 rotating days run */
    bool      days_dirty;          /* modified locally, not yet saved */
    SchedSlot slots[SCHED_MAX_SLOTS];
    int       num_slots;
    int       today_idx;           /* (today - 2024-01-01).days % 14 */
};

/* ── Run history ─────────────────────────────────────────────── */
#define HIST_MAX 14

struct HistEntry {
    char zone[32];
    char start_iso[24];   /* "YYYY-MM-DDTHH:MM:SS" */
    int  duration_sec;
    bool is_manual;
    bool completed;
};

struct HistData {
    bool     valid;
    HistEntry entries[HIST_MAX];
    int      count;
};

/* ── Pending HTTP action (set by UI callbacks, consumed in loop) ── */
enum PendingType {
    PENDING_NONE,
    PENDING_RUN_ZONE,
    PENDING_STOP_ALL,
    PENDING_FETCH_SCHEDULE,
    PENDING_FETCH_HISTORY,
    PENDING_SAVE_SCHEDULE,
    PENDING_SAVE_SCREENSHOT_SD
};

struct PendingAction {
    volatile PendingType type;
    char zone_name[32];     /* API name for PENDING_RUN_ZONE */
    int  run_minutes;       /* duration for PENDING_RUN_ZONE */
};

/* ── Full application state ──────────────────────────────── */
struct AppState {
    /* Operating mode */
    bool demo_mode;
    bool demo_pause_time;
    bool demo_pause_motion;
    bool controls_visible;

    /* Connectivity */
    bool wifi_connected;
    bool ws_connected;

    /* Zone relay states (from WebSocket) */
    ZoneRelays relays;

    /* Run state */
    CurrentRun current_run;
    NextRun    next_run;

    /* Sensor / weather */
    WeatherData weather;

    /* Local clock (updated every 200ms from NTP-synced RTC) */
    char time_str[10];     /* "HH:MM:SS" */
    char date_str[24];     /* "Saturday Mar 21" */

    /* Schedule & history */
    ScheduleData schedule;
    HistData     history;
    bool         data_loading;  /* true while HTTP fetch in progress */

    /* UI navigation: 0=dash, 1=schedule, 2=history */
    int  active_screen;
};

extern AppState      g_state;
extern PendingAction g_pending;
extern volatile uint32_t g_last_touch_ms;
