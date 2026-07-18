#pragma once
#include <Arduino.h>

/**
 * Shared application state — mutated only by the main/LVGL task. Network
 * workers return owned, bounded result buffers through a queue; the main task
 * validates and applies them before the LVGL update timer reads this state.
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
    bool  temp_valid;
    bool  humidity_valid;
    bool  wind_speed_valid;
    bool  wind_direction_valid;
    float temp_f;
    float humidity_pct;
    float wind_mph;
    char  wind_dir[4];
};

/* Measured irrigation telemetry.  Per-field validity avoids presenting a
 * missing sensor as a very convincing zero. */
struct IrrigationSensorData {
    bool valid;
    bool flow_valid;
    bool pressure_valid;
    bool soil_valid;
    bool rainfall_valid;
    bool rainfall_rate_valid;
    float flow_rate_lpm;
    float pressure_psi;
    float soil_moisture_percent;
    float rainfall_mm;
    float rainfall_rate_mm_hr;
    uint32_t updated_ms;
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
    bool      truncated;           /* UI omits server slots/sets beyond local display limits */
    bool      save_supported;      /* a lossless source document is available for PUT */
    uint8_t   source_num_slots;    /* total slots in the source document, including hidden ones */
    uint32_t  updated_ms;          /* millis() when the last valid schedule arrived */
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
    uint32_t updated_ms;
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

/* Last HTTP/action result.  The UI can render this without parsing Serial logs. */
struct ActionStatus {
    bool        busy;
    bool        success;
    PendingType type;
    int         http_code;          /* HTTP status, or a negative HTTPClient error */
    uint32_t    completed_ms;
    char        message[96];
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
    bool controls_auth_configured; /* a bearer token is present in this build */
    bool controls_authenticated;   /* most recent authenticated control request succeeded */
    uint32_t status_updated_ms;    /* last valid WebSocket status message */
    uint32_t weather_updated_ms;   /* last valid sensor/weather response */

    /* Zone relay states (from WebSocket) */
    ZoneRelays relays;

    /* Run state */
    CurrentRun current_run;
    NextRun    next_run;

    /* Sensor / weather */
    WeatherData weather;
    IrrigationSensorData irrigation;

    /* Local clock (updated every 200ms from NTP-synced RTC) */
    char time_str[10];     /* "HH:MM:SS" */
    char date_str[24];     /* "Saturday Mar 21" */

    /* Schedule & history */
    ScheduleData schedule;
    HistData     history;
    bool         data_loading;  /* true while HTTP fetch in progress */
    ActionStatus action;

    /* UI navigation: 0=dash, 1=schedule, 2=history */
    int  active_screen;
};

extern AppState      g_state;
extern PendingAction g_pending;
extern volatile uint32_t g_last_touch_ms;
