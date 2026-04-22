/**
 * main.cpp — LawnBot CrowPanel Display
 *
 * Demo mode boots with simulated data (no network needed).
 * Live mode connects to the LawnBot Controller Pi via WiFi + WebSocket.
 */

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "ui_theme.h"   /* ZONE_API_NAMES, zone_url_path() */

#if APP_MODE == APP_MODE_LIVE
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#endif

#include "app_state.h"
#include "lgfx.h"

#if ENABLE_SCREENSHOT_HTTP
#include "screenshot_server.h"
#endif
#if ENABLE_OTA
#include "ota_server.h"
#endif
#include "screenshot_serial.h"
#include "screenshot_sd.h"

/* ── Forward declarations from UI files ──────────────────── */
void ui_init();
void ui_set_splash_text(const char *text);
void ui_build_dashboard();
void ui_update_timer_cb(lv_timer_t *t);
void ui_set_snap_busy(bool busy);
void ui_show_toast(const char *text, uint32_t ms);

/* ── Global state ─────────────────────────────────────────── */
AppState      g_state   = {};
PendingAction g_pending = {PENDING_NONE, "", 0};

static const char *WIND_DIRS[8] = {"N","NE","E","SE","S","SW","W","NW"};

static uint32_t g_last_clock_ms  = 0;
static const uint32_t CTRL_WAKE_MS = 8000;

static void start_screenshot_sd_request() {
    if (screenshot_sd_start_save_latest()) {
        Serial.println("[SD] Screenshot save started");
        Serial0.println("[SD] Screenshot save started");
    } else {
        Serial.println("[SD] Screenshot save could not start");
        Serial0.println("[SD] Screenshot save could not start");
        ui_show_toast("SD SAVE FAILED", 3000);
        ui_set_snap_busy(false);
    }
    g_pending.type = PENDING_NONE;
}

static void poll_screenshot_sd_result() {
    bool ok = false;
    char path[48] = {};
    if (!screenshot_sd_poll_result(&ok, path, sizeof(path))) return;

    if (ok) {
        Serial.printf("[SD] Saved screenshot: %s\n", path);
        Serial0.printf("[SD] Saved screenshot: %s\n", path);
        char msg[80];
        snprintf(msg, sizeof(msg), "SAVED TO SD: %s", path);
        ui_show_toast(msg, 3000);
    } else {
        Serial.println("[SD] Screenshot save failed");
        Serial0.println("[SD] Screenshot save failed");
        ui_show_toast("SD SAVE FAILED", 3000);
    }
    ui_set_snap_busy(false);
}

#if APP_MODE == APP_MODE_LIVE
static WebSocketsClient ws;
static uint32_t g_last_sensor_ms  = 0;
static uint32_t g_last_hist_fetch = 0;
#else
/* ── Demo state ──────────────────────────────────────────── */
static time_t   g_demo_epoch_base           = DEMO_START_EPOCH;
static uint32_t g_demo_idle_ms              = 0;
static uint32_t g_demo_run_end_ms           = 0;
static uint8_t  g_demo_next_zone_idx        = 0;
static bool     g_demo_prev_pause_time      = false;
static bool     g_demo_prev_pause_motion    = false;
static uint32_t g_demo_time_paused_at_ms    = 0;
static uint32_t g_demo_time_paused_total_ms = 0;
static uint32_t g_demo_motion_paused_at_ms  = 0;
static uint32_t g_demo_motion_paused_total_ms = 0;
#endif

/* ── Generic helpers ─────────────────────────────────────── */
static void clear_relays() {
    g_state.relays.hanging_pots = false;
    g_state.relays.garden       = false;
    g_state.relays.misters      = false;
}

static void set_relay_for_zone(const char *zone) {
    clear_relays();
    if (strcmp(zone, "Hanging Pots") == 0) g_state.relays.hanging_pots = true;
    if (strcmp(zone, "Garden")       == 0) g_state.relays.garden       = true;
    if (strcmp(zone, "Misters")      == 0) g_state.relays.misters      = true;
}

static void format_clock_strings(time_t ep) {
    struct tm ti = {};
    localtime_r(&ep, &ti);
    strftime(g_state.time_str,  sizeof(g_state.time_str),  "%H:%M:%S", &ti);
    strftime(g_state.date_str,  sizeof(g_state.date_str),  "%A  %b %d", &ti);
}

static void set_next_run(time_t ep, const char *zone) {
    struct tm ti = {};
    localtime_r(&ep, &ti);
    g_state.next_run.valid = true;
    strlcpy(g_state.next_run.zone, zone, sizeof(g_state.next_run.zone));
    strftime(g_state.next_run.time_str, sizeof(g_state.next_run.time_str), "%H:%M", &ti);
    strftime(g_state.next_run.date_str, sizeof(g_state.next_run.date_str), "%Y-%m-%d", &ti);
}

/* Compute today's 14-day schedule index anchored to 2024-01-01 */
static int calc_today_sched_idx(time_t now_ep) {
    struct tm anchor_tm = {};
    anchor_tm.tm_year = 124; anchor_tm.tm_mon = 0; anchor_tm.tm_mday = 1;
    time_t anchor = mktime(&anchor_tm);
    int days = (int)((now_ep - anchor) / 86400);
    return ((days % SCHED_DAYS) + SCHED_DAYS) % SCHED_DAYS;
}

#if APP_MODE == APP_MODE_LIVE
/* ═══════════════════════════════════════════════════════════
   LIVE MODE — parsing & HTTP helpers
═══════════════════════════════════════════════════════════ */

static void parse_ws_status(const char *json, size_t len) {
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return;
    if (strcmp(doc["type"] | "", "status") != 0) return;

    JsonObject data = doc["data"];

    /* zone_states array */
    JsonArray zones = data["zone_states"];
    clear_relays();
    for (JsonObject z : zones) {
        const char *name = z["name"] | "";
        bool on = z["relay_on"] | false;
        if (strcmp(name, "Hanging Pots") == 0) g_state.relays.hanging_pots = on;
        else if (strcmp(name, "Garden")  == 0) g_state.relays.garden = on;
        else if (strcmp(name, "Misters") == 0) g_state.relays.misters = on;
    }

    /* current_run */
    JsonVariant run = data["current_run"];
    if (run.isNull() || !run.is<JsonObject>()) {
        g_state.current_run.active = false;
    } else {
        g_state.current_run.active = true;
        const char *zn = run["set_name"] | (run["name"] | "");
        strlcpy(g_state.current_run.zone, zn, sizeof(g_state.current_run.zone));
        g_state.current_run.remaining_sec = run["remaining_sec"] | (run["remaining_seconds"] | 0);
        g_state.current_run.total_sec     = run["total_sec"]     | (run["duration_seconds"]  | 0);
        g_state.current_run.is_manual     = run["is_manual"] | false;
    }

    /* next_run */
    JsonVariant next = data["next_run"];
    if (next.isNull() || !next.is<JsonObject>()) {
        g_state.next_run.valid = false;
    } else {
        g_state.next_run.valid = true;
        const char *zn = next["set_name"] | (next["name"] | "");
        strlcpy(g_state.next_run.zone, zn, sizeof(g_state.next_run.zone));
        const char *t = next["scheduled_time"] | (next["time"] | "");
        strlcpy(g_state.next_run.time_str, t, sizeof(g_state.next_run.time_str));
    }

    /* Update today's schedule index from status */
    int day_idx = data["schedule_day_index"] | -1;
    if (day_idx >= 0) g_state.schedule.today_idx = day_idx;
}

static void parse_schedule(const char *json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    const JsonObject &sched = doc.as<JsonObject>().containsKey("schedule_days")
                              ? doc.as<JsonObject>()
                              : doc["schedule"].as<JsonObject>();

    JsonArray days = sched["schedule_days"];
    if (!days.isNull()) {
        int n = 0;
        for (bool d : days) {
            if (n < SCHED_DAYS) g_state.schedule.days[n++] = d;
        }
    }

    JsonArray slots = sched["start_times"];
    g_state.schedule.num_slots = 0;
    for (JsonObject slot : slots) {
        if (g_state.schedule.num_slots >= SCHED_MAX_SLOTS) break;
        SchedSlot &s = g_state.schedule.slots[g_state.schedule.num_slots];
        strlcpy(s.time_str, slot["time"] | "", sizeof(s.time_str));
        s.enabled   = slot["enabled"] | true;
        s.num_zones = 0;
        JsonArray sets = slot["sets"];
        for (JsonObject set : sets) {
            if (s.num_zones >= 3) break;
            SchedZone &z = s.zones[s.num_zones];
            strlcpy(z.name, set["name"] | "", sizeof(z.name));
            z.duration_min = set["duration_minutes"] | 0.0f;
            z.enabled      = set["enabled"] | true;
            s.num_zones++;
        }
        g_state.schedule.num_slots++;
    }

    g_state.schedule.valid       = true;
    g_state.schedule.days_dirty  = false;
    g_state.data_loading         = false;
}

static void parse_history(const char *json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    JsonArray arr = doc.as<JsonArray>();
    g_state.history.count = 0;
    for (JsonObject entry : arr) {
        if (g_state.history.count >= HIST_MAX) break;
        HistEntry &e = g_state.history.entries[g_state.history.count];
        const char *zn = entry["set_name"] | (entry["name"] | "");
        strlcpy(e.zone,       zn,                   sizeof(e.zone));
        strlcpy(e.start_iso,  entry["start_time"] | "", sizeof(e.start_iso));
        e.duration_sec = entry["duration_seconds"] | 0;
        e.is_manual    = entry["is_manual"]  | false;
        e.completed    = entry["completed"]  | true;
        g_state.history.count++;
    }
    g_state.history.valid = true;
    g_state.data_loading  = false;
}

static String build_schedule_put_body() {
    JsonDocument doc;
    JsonObject sched = doc["schedule"].to<JsonObject>();
    JsonArray days = sched["schedule_days"].to<JsonArray>();
    for (int i = 0; i < SCHED_DAYS; i++) days.add(g_state.schedule.days[i]);

    JsonArray slots = sched["start_times"].to<JsonArray>();
    for (int s = 0; s < g_state.schedule.num_slots; s++) {
        const SchedSlot &slot = g_state.schedule.slots[s];
        JsonObject o = slots.add<JsonObject>();
        o["time"]    = slot.time_str;
        o["enabled"] = slot.enabled;
        JsonArray sets = o["sets"].to<JsonArray>();
        for (int z = 0; z < slot.num_zones; z++) {
            JsonObject zo = sets.add<JsonObject>();
            zo["name"]             = slot.zones[z].name;
            zo["duration_minutes"] = slot.zones[z].duration_min;
            zo["enabled"]          = slot.zones[z].enabled;
            zo["mode"]             = "normal";
        }
    }
    String body;
    serializeJson(doc, body);
    return body;
}

static void poll_sensors() {
    HTTPClient http;
    http.setTimeout(5000);
    http.begin(LAWNBOT_API_BASE "/sensors/latest");
    if (http.GET() == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
            JsonObject env = doc["environment"];
            if (!env.isNull()) {
                float tc = env["temperature_c"] | -999.0f;
                if (tc > -999.0f) {
                    g_state.weather.valid        = true;
                    g_state.weather.temp_f       = tc * 9.0f / 5.0f + 32.0f;
                    g_state.weather.humidity_pct = env["humidity_percent"] | 0.0f;
                    g_state.weather.wind_mph     = (env["wind_speed_ms"] | 0.0f) * 2.23694f;
                    strlcpy(g_state.weather.wind_dir,
                            env["wind_direction_compass"] | "--",
                            sizeof(g_state.weather.wind_dir));
                }
            }
        }
    }
    http.end();
}

static void execute_live_action() {
    HTTPClient http;
    http.setTimeout(6000);
    String url;

    switch (g_pending.type) {
        case PENDING_RUN_ZONE: {
            /* URL-encode zone name (e.g. "Hanging Pots" → "Hanging%20Pots") */
            url = String(LAWNBOT_API_BASE) + "/zones/"
                + zone_url_path(g_pending.zone_name) + "/run";
            http.begin(url);
            http.addHeader("Content-Type", "application/json");
            char body[64];
            snprintf(body, sizeof(body), "{\"duration_minutes\":%d}",
                     g_pending.run_minutes > 0 ? g_pending.run_minutes : DEFAULT_RUN_MINUTES);
            int code = http.POST(body);
            Serial.printf("[HTTP] POST run %s (%d min) -> %d\n",
                          g_pending.zone_name, g_pending.run_minutes, code);
            break;
        }
        case PENDING_STOP_ALL:
            http.begin(String(LAWNBOT_API_BASE) + "/stop-all");
            http.addHeader("Content-Type", "application/json");
            Serial.printf("[HTTP] POST stop-all -> %d\n", http.POST(""));
            break;

        case PENDING_FETCH_SCHEDULE:
            g_state.data_loading = true;
            http.begin(String(LAWNBOT_API_BASE) + "/schedule");
            if (http.GET() == 200) parse_schedule(http.getString().c_str());
            else g_state.data_loading = false;
            break;

        case PENDING_FETCH_HISTORY:
            g_state.data_loading = true;
            http.begin(String(LAWNBOT_API_BASE) + "/history?limit=14");
            if (http.GET() == 200) parse_history(http.getString().c_str());
            else g_state.data_loading = false;
            break;

        case PENDING_SAVE_SCHEDULE: {
            http.begin(String(LAWNBOT_API_BASE) + "/schedule");
            http.addHeader("Content-Type", "application/json");
            String body = build_schedule_put_body();
            int code = http.PUT(body);
            Serial.printf("[HTTP] PUT schedule -> %d\n", code);
            if (code == 200) g_state.schedule.days_dirty = false;
            break;
        }

        default: break;
    }

    http.end();
    g_pending.type = PENDING_NONE;
}

static void ws_event(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED: g_state.ws_connected = false; break;
        case WStype_CONNECTED:    g_state.ws_connected = true;  break;
        case WStype_TEXT:
            parse_ws_status(reinterpret_cast<const char *>(payload), length);
            break;
        default: break;
    }
}

static void connect_wifi() {
    Serial.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
    Serial0.printf("[WiFi] Connecting to %s\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
#if ENABLE_OTA
    WiFi.setHostname(OTA_HOSTNAME);
#endif
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500); lv_timer_handler(); attempts++;
    }
    g_state.wifi_connected = (WiFi.status() == WL_CONNECTED);
    ui_set_splash_text(g_state.wifi_connected ? "WiFi connected" : "WiFi failed");
    if (g_state.wifi_connected) {
        Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
        Serial0.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] Connection failed");
        Serial0.println("[WiFi] Connection failed");
    }
}

static void sync_ntp() {
    if (!g_state.wifi_connected) return;
    configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
    struct tm ti = {};
    for (int i = 0; i < 20 && !getLocalTime(&ti, 0); i++) {
        delay(250); lv_timer_handler();
    }
}

static void update_live_clock() {
    if (millis() - g_last_clock_ms < 200) return;
    g_last_clock_ms = millis();
    struct tm ti = {};
    if (getLocalTime(&ti, 0)) {
        strftime(g_state.time_str, sizeof(g_state.time_str), "%H:%M:%S", &ti);
        strftime(g_state.date_str, sizeof(g_state.date_str), "%A  %b %d", &ti);

        /* Keep today_idx up-to-date */
        time_t now; time(&now);
        g_state.schedule.today_idx = calc_today_sched_idx(now);
    }
}

#else
/* ═══════════════════════════════════════════════════════════
   DEMO MODE
═══════════════════════════════════════════════════════════ */

static uint32_t demo_time_ms(uint32_t raw) {
    return g_state.demo_pause_time
        ? (g_demo_time_paused_at_ms - g_demo_time_paused_total_ms)
        : (raw - g_demo_time_paused_total_ms);
}
static uint32_t demo_motion_ms(uint32_t raw) {
    return g_state.demo_pause_motion
        ? (g_demo_motion_paused_at_ms - g_demo_motion_paused_total_ms)
        : (raw - g_demo_motion_paused_total_ms);
}

static void stop_demo_run() {
    g_state.current_run = {};
    clear_relays();
    g_demo_idle_ms = demo_motion_ms(millis());
}

static void start_demo_run(const char *zone, uint32_t dur_sec, bool manual) {
    g_state.current_run.active        = true;
    strlcpy(g_state.current_run.zone, zone, sizeof(g_state.current_run.zone));
    g_state.current_run.total_sec     = dur_sec;
    g_state.current_run.remaining_sec = dur_sec;
    g_state.current_run.is_manual     = manual;
    set_relay_for_zone(zone);
    g_demo_run_end_ms = demo_motion_ms(millis()) + dur_sec * 1000UL;
    int idx = 0;
    for (int i = 0; i < 3; i++) if (strcmp(zone, ZONE_API_NAMES[i]) == 0) { idx = i; break; }
    g_demo_next_zone_idx = (idx + 1) % 3;
}

static void update_demo_weather(uint32_t ms) {
    uint32_t step = ms / 15000UL;
    float t = (float)step;
    g_state.weather.valid        = true;
    g_state.weather.temp_f       = 74.0f + sinf(t * 0.25f) * 5.0f;
    g_state.weather.humidity_pct = 52.0f + sinf(t * 0.19f + 0.8f) * 8.0f;
    g_state.weather.wind_mph     =  5.0f + sinf(t * 0.31f + 1.5f) * 2.5f;
    strlcpy(g_state.weather.wind_dir, WIND_DIRS[step % 8], sizeof(g_state.weather.wind_dir));
}

static void seed_demo_schedule() {
    g_state.schedule.valid       = true;
    g_state.schedule.days_dirty  = false;
    /* Alternating days: 0,2,4,6,8,10,12 active */
    for (int i = 0; i < SCHED_DAYS; i++) g_state.schedule.days[i] = (i % 2 == 0);
    g_state.schedule.today_idx   = calc_today_sched_idx(DEMO_START_EPOCH);

    /* Two start times */
    g_state.schedule.num_slots = 2;

    SchedSlot &s0 = g_state.schedule.slots[0];
    strlcpy(s0.time_str, "06:00", sizeof(s0.time_str));
    s0.enabled   = true;
    s0.num_zones = 2;
    strlcpy(s0.zones[0].name, "Hanging Pots", sizeof(s0.zones[0].name));
    s0.zones[0].duration_min = 10.0f; s0.zones[0].enabled = true;
    strlcpy(s0.zones[1].name, "Garden", sizeof(s0.zones[1].name));
    s0.zones[1].duration_min = 15.0f; s0.zones[1].enabled = true;

    SchedSlot &s1 = g_state.schedule.slots[1];
    strlcpy(s1.time_str, "09:00", sizeof(s1.time_str));
    s1.enabled   = true;
    s1.num_zones = 1;
    strlcpy(s1.zones[0].name, "Misters", sizeof(s1.zones[0].name));
    s1.zones[0].duration_min = 30.0f; s1.zones[0].enabled = true;
}

static void seed_demo_history() {
    g_state.history.valid = true;
    g_state.history.count = 0;

    /* Generate 10 fake history entries going back in time */
    static const char *ZONES[3] = {"Hanging Pots", "Garden", "Misters"};
    static const int   DURS[3]  = {600, 900, 300};
    time_t base = DEMO_START_EPOCH - 86400; /* yesterday */
    for (int i = 0; i < 10; i++) {
        HistEntry &e = g_state.history.entries[g_state.history.count++];
        strlcpy(e.zone, ZONES[i % 3], sizeof(e.zone));
        time_t t = base - i * 86400 + 6 * 3600; /* 06:00 each day */
        struct tm ti = {};
        localtime_r(&t, &ti);
        strftime(e.start_iso, sizeof(e.start_iso), "%Y-%m-%dT%H:%M:%S", &ti);
        e.duration_sec = DURS[i % 3];
        e.is_manual    = (i == 2 || i == 5);
        e.completed    = (i != 4);
    }
}

static void execute_demo_action() {
    switch (g_pending.type) {
        case PENDING_RUN_ZONE:
            if (!g_state.current_run.active)
                start_demo_run(g_pending.zone_name, g_pending.run_minutes * 60, true);
            break;
        case PENDING_STOP_ALL:
            stop_demo_run();
            break;
        case PENDING_FETCH_SCHEDULE:
            /* Already seeded; nothing to do */
            g_state.data_loading = false;
            break;
        case PENDING_FETCH_HISTORY:
            g_state.data_loading = false;
            break;
        case PENDING_SAVE_SCHEDULE:
            g_state.schedule.days_dirty = false;
            break;
        default: break;
    }
    g_pending.type = PENDING_NONE;
}

static void update_demo_state() {
    uint32_t raw = millis();
    g_state.controls_visible = (raw - g_last_touch_ms) < CTRL_WAKE_MS;

    /* Pause tracking */
    if (g_state.demo_pause_time && !g_demo_prev_pause_time)
        g_demo_time_paused_at_ms = raw;
    else if (!g_state.demo_pause_time && g_demo_prev_pause_time)
        g_demo_time_paused_total_ms += raw - g_demo_time_paused_at_ms;
    g_demo_prev_pause_time = g_state.demo_pause_time;

    if (g_state.demo_pause_motion && !g_demo_prev_pause_motion)
        g_demo_motion_paused_at_ms = raw;
    else if (!g_state.demo_pause_motion && g_demo_prev_pause_motion)
        g_demo_motion_paused_total_ms += raw - g_demo_motion_paused_at_ms;
    g_demo_prev_pause_motion = g_state.demo_pause_motion;

    uint32_t dt_ms  = demo_time_ms(raw);
    uint32_t mot_ms = demo_motion_ms(raw);
    time_t now_ep   = g_demo_epoch_base + dt_ms / 1000UL;

    if (raw - g_last_clock_ms >= 1000) {
        g_last_clock_ms = raw;
        format_clock_strings(now_ep);
        g_state.schedule.today_idx = calc_today_sched_idx(now_ep);
    }

    update_demo_weather(mot_ms);

    if (g_state.current_run.active) {
        int32_t rem = (int32_t)(g_demo_run_end_ms - mot_ms);
        if (rem <= 0) stop_demo_run();
        else g_state.current_run.remaining_sec = (rem + 999) / 1000;
    } else if (mot_ms - g_demo_idle_ms >= DEMO_AUTOCYCLE_SECONDS * 1000UL) {
        start_demo_run(ZONE_API_NAMES[g_demo_next_zone_idx],
                       DEMO_AUTORUN_SECONDS, false);
    }

    set_next_run(now_ep + (g_state.current_run.active ? 20*60 : 12*60),
                 ZONE_API_NAMES[g_demo_next_zone_idx]);
}

static void seed_demo_state() {
    g_state.demo_mode         = true;
    g_state.demo_pause_time   = false;
    g_state.demo_pause_motion = true;
    g_state.controls_visible  = false;
    g_state.wifi_connected    = false;
    g_state.ws_connected      = false;
    clear_relays();
    stop_demo_run();
    g_demo_idle_ms = 0;
    seed_demo_schedule();
    seed_demo_history();
    update_demo_state();
}
#endif

/* ═══════════════════════════════════════════════════════════
   SETUP
═══════════════════════════════════════════════════════════ */
void setup() {
    Serial.begin(115200);
    /* CH340 USB-UART bridge uses ESP32-S3 UART0 on GPIO44(RX0)/GPIO43(TX0) */
    Serial0.begin(115200, SERIAL_8N1, 44, 43);
    delay(200);
    Serial.println("\n[Boot] LawnBot CrowPanel Display");
    Serial0.println("\n[Boot] LawnBot CrowPanel Display");

    lcd.setup();
    ui_init();
    lv_timer_handler();
    if (screenshot_sd_init()) {
        Serial.println("[SD] TF card ready");
        Serial0.println("[SD] TF card ready");
    } else {
        Serial.println("[SD] TF card not ready");
        Serial0.println("[SD] TF card not ready");
    }

#if APP_MODE == APP_MODE_LIVE
    g_state.demo_mode = false;
    ui_set_splash_text("Connecting to LawnBot...");
    connect_wifi();
    lv_timer_handler();
    sync_ntp();
    lv_timer_handler();

    /* Fetch initial schedule */
    if (g_state.wifi_connected) {
        ui_set_splash_text("Loading schedule...");
        HTTPClient http;
        http.setTimeout(5000);
        http.begin(String(LAWNBOT_API_BASE) + "/schedule");
        if (http.GET() == 200) parse_schedule(http.getString().c_str());
        http.end();

        /* Fetch initial history */
        http.begin(String(LAWNBOT_API_BASE) + "/history?limit=14");
        if (http.GET() == 200) parse_history(http.getString().c_str());
        http.end();

        poll_sensors();
    }

#if ENABLE_OTA
    if (g_state.wifi_connected) ota_server_init();
#endif

#else
    ui_set_splash_text("Starting demo mode...");
    seed_demo_state();
    delay(350);
    lv_timer_handler();
#endif

    ui_set_splash_text("Loading dashboard...");
    ui_build_dashboard();
    lv_timer_handler();

    lv_timer_create(ui_update_timer_cb, 500, nullptr);

#if APP_MODE == APP_MODE_LIVE
    ws.begin(LAWNBOT_HOST, LAWNBOT_PORT, LAWNBOT_WS_PATH);
    ws.onEvent(ws_event);
    ws.setReconnectInterval(5000);
#else
    update_demo_state();
#endif

#if ENABLE_SCREENSHOT_HTTP
    screenshot_server_init();
#endif

    Serial.println("[Boot] Ready");
    Serial.println("[Serial] Screen grab over USB: type capture + Enter, or run:");
    Serial.println("         python tools/fetch_screenshot_serial.py COM3");
    Serial0.println("[Boot] Ready");
    Serial0.println("[UART] Screen grab over CH340/UART: send 'capture' + Enter");
}

/* ═══════════════════════════════════════════════════════════
   LOOP
═══════════════════════════════════════════════════════════ */
void loop() {
#if APP_MODE == APP_MODE_LIVE
    ws.loop();
    update_live_clock();

    /* Periodic sensor poll every 15 s */
    uint32_t now = millis();
    if (now - g_last_sensor_ms > 15000UL) {
        g_last_sensor_ms = now;
        poll_sensors();
    }

    /* WiFi reconnect check */
    static uint32_t last_wifi_ms = 0;
    if (now - last_wifi_ms > 10000UL) {
        last_wifi_ms = now;
        bool ok = (WiFi.status() == WL_CONNECTED);
        if (ok != g_state.wifi_connected) {
            g_state.wifi_connected = ok;
            if (!ok) WiFi.reconnect();
        }
    }
#else
    update_demo_state();
#endif

    g_state.controls_visible = (millis() - g_last_touch_ms) < CTRL_WAKE_MS;
    lv_timer_handler();
    poll_screenshot_sd_result();

    if (g_pending.type != PENDING_NONE) {
        if (g_pending.type == PENDING_SAVE_SCREENSHOT_SD) {
            start_screenshot_sd_request();
        } else {
#if APP_MODE == APP_MODE_LIVE
            execute_live_action();
#else
            execute_demo_action();
#endif
        }
    }

#if ENABLE_SCREENSHOT_HTTP
    screenshot_server_loop();
#endif
#if ENABLE_OTA
    ota_server_loop();
#endif
    screenshot_serial_poll();

    delay(5);
}
