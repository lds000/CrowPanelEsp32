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
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#endif

#include "app_state.h"
#include "lgfx.h"
#include "time_helpers.h"

#if ENABLE_SCREENSHOT_HTTP
#include "screenshot_server.h"
#endif
#if ENABLE_OTA
#include "ota_server.h"
#endif
#include "screenshot_serial.h"
#if ENABLE_SCREENSHOT_SD
#include "screenshot_sd.h"
#endif

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

#if ENABLE_SCREENSHOT_SD
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
#endif

#if APP_MODE == APP_MODE_LIVE
static WebSocketsClient ws;
static uint32_t g_last_sensor_ms  = 0;
static uint32_t g_last_hist_fetch = 0;
/* The full schedule is retained so edits can be applied without discarding
 * fields or zone sets this compact UI does not display. */
static String   g_schedule_source_json;
static String   g_pending_save_body;
static String   g_pending_save_source;

enum NetOperation : uint8_t {
    NET_GET_SENSORS,
    NET_GET_SCHEDULE,
    NET_GET_HISTORY,
    NET_RUN_ZONE,
    NET_STOP_ALL,
    NET_SAVE_PREFLIGHT,
    NET_PUT_SCHEDULE
};

enum NetResultError : uint8_t {
    NET_RESULT_OK,
    NET_RESULT_WIFI_DOWN,
    NET_RESULT_BODY_TOO_LARGE,
    NET_RESULT_OUT_OF_MEMORY,
    NET_RESULT_READ_FAILED,
    NET_RESULT_CANCELLED_BY_STOP
};

struct NetRequest {
    NetOperation op;
    PendingType action;
    bool report_action;
    char zone_name[32];
    int run_minutes;
    char *payload;
    size_t payload_len;
};

struct NetResult {
    NetOperation op;
    PendingType action;
    bool report_action;
    int http_code;
    NetResultError error;
    char *body;
    size_t body_len;
};

static QueueHandle_t g_net_request_queue = nullptr;
static QueueHandle_t g_net_result_queue = nullptr;
static TaskHandle_t  g_net_worker_task = nullptr;
static uint32_t      g_pending_action_mask = 0;
static volatile uint32_t g_cancelled_action_mask = 0;
static bool          g_sensor_request_pending = false;
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

static bool set_next_run_time_text(const char *text) {
    return lawnbot_time::parse_next_run_time_text(
        text, g_state.next_run.time_str, sizeof(g_state.next_run.time_str),
        g_state.next_run.date_str, sizeof(g_state.next_run.date_str));
}

/* Compute today's 14-day schedule index anchored to local 2024-01-01. */
static int calc_today_sched_idx(time_t now_ep) {
    struct tm local = {};
    localtime_r(&now_ep, &local);
    const int index = lawnbot_time::schedule_index_for_civil_date(
        local.tm_year + 1900, static_cast<unsigned>(local.tm_mon + 1),
        static_cast<unsigned>(local.tm_mday), SCHED_DAYS);
    return index >= 0 ? index : 0;
}

#if APP_MODE == APP_MODE_LIVE
/* ═══════════════════════════════════════════════════════════
   LIVE MODE — parsing & HTTP helpers
═══════════════════════════════════════════════════════════ */

static bool http_success(int code) {
    return code >= 200 && code < 300;
}

static bool bearer_token_configured() {
    return LAWNBOT_API_BEARER_TOKEN[0] != '\0';
}

static bool add_control_auth(HTTPClient &http) {
    if (!bearer_token_configured()) return false;
    String value = F("Bearer ");
    value += LAWNBOT_API_BEARER_TOKEN;
    http.addHeader("Authorization", value);
    return true;
}

static void begin_action(PendingType type, const char *message) {
    g_state.action.busy = true;
    g_state.action.success = false;
    g_state.action.type = type;
    g_state.action.http_code = 0;
    g_state.action.completed_ms = 0;
    strlcpy(g_state.action.message, message ? message : "WORKING", sizeof(g_state.action.message));
}

static void finish_action(PendingType type, bool success, int code, const char *message) {
    g_state.action.busy = false;
    g_state.action.success = success;
    g_state.action.type = type;
    g_state.action.http_code = code;
    g_state.action.completed_ms = millis();
    strlcpy(g_state.action.message, message ? message : (success ? "OK" : "FAILED"),
            sizeof(g_state.action.message));
}

static String url_encode_path_segment(const char *value) {
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    String encoded;
    if (!value) return encoded;
    encoded.reserve(strlen(value) * 3U);
    for (const uint8_t *p = reinterpret_cast<const uint8_t *>(value); *p; ++p) {
        const uint8_t c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += HEX_DIGITS[c >> 4];
            encoded += HEX_DIGITS[c & 0x0F];
        }
    }
    return encoded;
}

static void invalidate_live_status() {
    clear_relays();
    g_state.current_run = {};
    g_state.next_run = {};
    g_state.status_updated_ms = 0;
}

static bool canonical_schedule_source(const char *json, size_t len, String &source) {
    if (!json || len == 0 || len > LAWNBOT_MAX_SCHEDULE_JSON_BYTES) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok ||
        !doc.is<JsonObject>()) return false;
    JsonObject root = doc.as<JsonObject>();
    JsonObject sched = root["schedule"].is<JsonObject>()
                     ? root["schedule"].as<JsonObject>() : root;
    if (!sched["schedule_days"].is<JsonArray>() || !sched["start_times"].is<JsonArray>())
        return false;
    const size_t size = measureJson(sched);
    if (size == 0 || size > LAWNBOT_MAX_SCHEDULE_JSON_BYTES) return false;
    source = "";
    source.reserve(size + 1U);
    return serializeJson(sched, source) == size;
}

class BoundedResponseStream : public Stream {
public:
    BoundedResponseStream(char *buffer, size_t capacity)
        : buffer_(buffer), capacity_(capacity) {}

    size_t write(uint8_t value) override {
        return write(&value, 1);
    }

    size_t write(const uint8_t *data, size_t size) override {
        if (!data || size == 0) return 0;
        if (length_ + size > capacity_) {
            overflowed_ = true;
            return 0;
        }
        memcpy(buffer_ + length_, data, size);
        length_ += size;
        return size;
    }

    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}

    size_t length() const { return length_; }
    bool overflowed() const { return overflowed_; }

private:
    char *buffer_ = nullptr;
    size_t capacity_ = 0;
    size_t length_ = 0;
    bool overflowed_ = false;
};

static bool worker_read_body(HTTPClient &http, size_t max_bytes, NetResult &result) {
    const int announced = http.getSize();
    if (announced > 0 && static_cast<size_t>(announced) > max_bytes) {
        result.error = NET_RESULT_BODY_TOO_LARGE;
        return false;
    }

    const size_t capacity = announced > 0 ? static_cast<size_t>(announced) : max_bytes;
    char *buffer = static_cast<char *>(malloc(capacity + 1U));
    if (!buffer) {
        result.error = NET_RESULT_OUT_OF_MEMORY;
        return false;
    }

    BoundedResponseStream sink(buffer, capacity);
    const int written = http.writeToStream(&sink);
    if (sink.overflowed()) {
        free(buffer);
        result.error = NET_RESULT_BODY_TOO_LARGE;
        return false;
    }
    if (written < 0) {
        free(buffer);
        result.error = NET_RESULT_READ_FAILED;
        return false;
    }

    buffer[sink.length()] = '\0';
    result.body = buffer;
    result.body_len = sink.length();
    return true;
}

static void network_worker(void * /*parameter*/) {
    NetRequest request = {};
    for (;;) {
        if (xQueueReceive(g_net_request_queue, &request, portMAX_DELAY) != pdTRUE) continue;

        NetResult result = {};
        result.op = request.op;
        result.action = request.action;
        result.report_action = request.report_action;

        if (request.action != PENDING_NONE &&
            (g_cancelled_action_mask & (1UL << static_cast<unsigned>(request.action))) != 0) {
            result.error = NET_RESULT_CANCELLED_BY_STOP;
        } else if (WiFi.status() != WL_CONNECTED) {
            result.error = NET_RESULT_WIFI_DOWN;
        } else {
            HTTPClient http;
            http.setTimeout(request.op == NET_GET_SENSORS ? 3500 : 6000);
            String url;

            switch (request.op) {
                case NET_GET_SENSORS:
                    http.begin(LAWNBOT_API_BASE "/sensors/latest");
                    result.http_code = http.GET();
                    if (http_success(result.http_code)) worker_read_body(http, 8192, result);
                    break;

                case NET_GET_SCHEDULE:
                case NET_SAVE_PREFLIGHT:
                    http.begin(String(LAWNBOT_API_BASE) + "/schedule");
                    result.http_code = http.GET();
                    if (http_success(result.http_code))
                        worker_read_body(http, LAWNBOT_MAX_SCHEDULE_JSON_BYTES, result);
                    break;

                case NET_GET_HISTORY:
                    http.begin(String(LAWNBOT_API_BASE) + "/history?limit=14");
                    result.http_code = http.GET();
                    if (http_success(result.http_code))
                        worker_read_body(http, LAWNBOT_MAX_HISTORY_JSON_BYTES, result);
                    break;

                case NET_RUN_ZONE: {
                    url = String(LAWNBOT_API_BASE) + "/zones/"
                        + url_encode_path_segment(request.zone_name) + "/run";
                    http.begin(url);
                    http.addHeader("Content-Type", "application/json");
                    add_control_auth(http);
                    char body[64];
                    snprintf(body, sizeof(body), "{\"duration_minutes\":%d}",
                             request.run_minutes);
                    result.http_code = http.POST(body);
                    break;
                }

                case NET_STOP_ALL:
                    http.begin(String(LAWNBOT_API_BASE) + "/stop-all");
                    http.addHeader("Content-Type", "application/json");
                    add_control_auth(http);
                    result.http_code = http.POST("");
                    break;

                case NET_PUT_SCHEDULE:
                    http.begin(String(LAWNBOT_API_BASE) + "/schedule");
                    http.addHeader("Content-Type", "application/json");
                    add_control_auth(http);
                    result.http_code = http.PUT(
                        reinterpret_cast<uint8_t *>(request.payload), request.payload_len);
                    break;
            }
            http.end();
        }

        if (request.payload) {
            free(request.payload);
            request.payload = nullptr;
        }
        xQueueSend(g_net_result_queue, &result, portMAX_DELAY);
    }
}

static bool network_worker_init() {
    g_net_request_queue = xQueueCreate(6, sizeof(NetRequest));
    g_net_result_queue = xQueueCreate(6, sizeof(NetResult));
    if (!g_net_request_queue || !g_net_result_queue ||
        xTaskCreatePinnedToCore(network_worker, "lawn-http", 8192, nullptr, 1,
                                &g_net_worker_task, 0) != pdPASS) {
        if (g_net_request_queue) vQueueDelete(g_net_request_queue);
        if (g_net_result_queue) vQueueDelete(g_net_result_queue);
        g_net_request_queue = nullptr;
        g_net_result_queue = nullptr;
        g_net_worker_task = nullptr;
        return false;
    }
    return true;
}

static bool parse_ws_status(const char *json, size_t len) {
    if (!json || len == 0 || len > LAWNBOT_MAX_WS_JSON_BYTES) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
    if (strcmp(doc["type"] | "", "status") != 0) return false;

    JsonObject data = doc["data"];
    if (data.isNull() || !data["zone_states"].is<JsonArray>()) return false;

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
        const char *zn = next["set_name"] | (next["name"] | "");
        strlcpy(g_state.next_run.zone, zn, sizeof(g_state.next_run.zone));
        const char *t = next["scheduled_time"] | (next["time"] | "");
        g_state.next_run.valid = set_next_run_time_text(t);
    }

    /* Update today's schedule index from status */
    int day_idx = data["schedule_day_index"] | -1;
    if (day_idx >= 0 && day_idx < SCHED_DAYS) g_state.schedule.today_idx = day_idx;
    g_state.status_updated_ms = millis();
    return true;
}

static bool parse_schedule(const char *json, size_t len) {
    if (!json || len == 0 || len > LAWNBOT_MAX_SCHEDULE_JSON_BYTES) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok ||
        !doc.is<JsonObject>()) return false;

    JsonObject root = doc.as<JsonObject>();
    JsonObject sched = root["schedule"].is<JsonObject>()
                     ? root["schedule"].as<JsonObject>() : root;
    JsonArray days = sched["schedule_days"].as<JsonArray>();
    JsonArray slots = sched["start_times"].as<JsonArray>();
    if (days.isNull() || days.size() != SCHED_DAYS || slots.isNull()) return false;

    ScheduleData parsed = {};
    parsed.today_idx = g_state.schedule.today_idx;
    int day_index = 0;
    for (JsonVariant day : days) {
        if (!day.is<bool>()) return false;
        parsed.days[day_index++] = day.as<bool>();
    }

    const size_t source_slots = slots.size();
    parsed.source_num_slots = static_cast<uint8_t>(source_slots > 255 ? 255 : source_slots);
    parsed.truncated = source_slots > SCHED_MAX_SLOTS;

    for (JsonObject slot : slots) {
        if (parsed.num_slots >= SCHED_MAX_SLOTS) break;
        const char *time_text = slot["time"] | "";
        if (!time_text[0]) return false;

        SchedSlot &target = parsed.slots[parsed.num_slots];
        strlcpy(target.time_str, time_text, sizeof(target.time_str));
        target.enabled = slot["enabled"] | true;
        JsonArray sets = slot["sets"].as<JsonArray>();
        if (sets.isNull()) return false;
        if (sets.size() > 3) parsed.truncated = true;
        for (JsonObject set : sets) {
            if (target.num_zones >= 3) break;
            const char *name = set["name"] | "";
            if (!name[0]) return false;
            SchedZone &zone = target.zones[target.num_zones++];
            strlcpy(zone.name, name, sizeof(zone.name));
            zone.duration_min = set["duration_minutes"] | 0.0f;
            zone.enabled = set["enabled"] | true;
        }
        parsed.num_slots++;
    }

    String preserved;
    if (!canonical_schedule_source(json, len, preserved)) return false;

    parsed.valid = true;
    parsed.days_dirty = false;
    parsed.save_supported = true;
    parsed.updated_ms = millis();
    g_schedule_source_json = preserved;
    g_state.schedule = parsed;
    return true;
}

static bool parse_history(const char *json, size_t len) {
    if (!json || len == 0 || len > LAWNBOT_MAX_HISTORY_JSON_BYTES) return false;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok ||
        !doc.is<JsonArray>()) return false;

    HistData parsed = {};
    for (JsonObject entry : doc.as<JsonArray>()) {
        if (parsed.count >= HIST_MAX) break;
        const char *zn = entry["set_name"] | (entry["name"] | "");
        const char *start = entry["start_time"] | "";
        if (!zn[0] || !start[0]) continue;
        HistEntry &e = parsed.entries[parsed.count++];
        strlcpy(e.zone, zn, sizeof(e.zone));
        strlcpy(e.start_iso, start, sizeof(e.start_iso));
        e.duration_sec = entry["duration_seconds"] | 0;
        e.is_manual = entry["is_manual"] | false;
        e.completed = entry["completed"] | true;
    }
    parsed.valid = true;
    parsed.updated_ms = millis();
    g_state.history = parsed;
    return true;
}

/* Apply only fields the panel edits to the full fetched schedule.  Every
 * unknown setting and every hidden zone set survives byte-for-byte semantically. */
static bool build_schedule_put_body(String &body, String &updated_source,
                                    char *error, size_t error_size) {
    if (!g_state.schedule.save_supported || g_schedule_source_json.isEmpty()) {
        strlcpy(error, "REFRESH SCHEDULE BEFORE SAVING", error_size);
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, g_schedule_source_json) != DeserializationError::Ok ||
        !doc.is<JsonObject>()) {
        strlcpy(error, "SCHEDULE SOURCE IS INVALID", error_size);
        return false;
    }
    JsonObject sched = doc.as<JsonObject>();
    JsonArray days = sched["schedule_days"].as<JsonArray>();
    JsonArray slots = sched["start_times"].as<JsonArray>();
    if (days.size() != SCHED_DAYS || slots.isNull()) {
        strlcpy(error, "SCHEDULE SCHEMA CHANGED; REFRESH REQUIRED", error_size);
        return false;
    }

    const int visible_source_slots = static_cast<int>(slots.size() > SCHED_MAX_SLOTS
                                    ? SCHED_MAX_SLOTS : slots.size());
    if (g_state.schedule.num_slots < visible_source_slots ||
        (slots.size() > SCHED_MAX_SLOTS && g_state.schedule.num_slots != SCHED_MAX_SLOTS)) {
        strlcpy(error, "UNSUPPORTED SCHEDULE STRUCTURE CHANGE", error_size);
        return false;
    }

    for (int i = 0; i < SCHED_DAYS; ++i) days[i].set(g_state.schedule.days[i]);
    for (int i = 0; i < visible_source_slots; ++i) {
        JsonObject slot = slots[i].as<JsonObject>();
        slot["time"] = g_state.schedule.slots[i].time_str;
        slot["enabled"] = g_state.schedule.slots[i].enabled;
    }

    /* New times clone the complete first source slot, including hidden sets.
     * Refuse if there is no lossless template instead of inventing a partial one. */
    if (g_state.schedule.num_slots > static_cast<int>(slots.size())) {
        if (slots.isNull() || slots.size() == 0) {
            strlcpy(error, "CANNOT ADD FIRST SLOT SAFELY", error_size);
            return false;
        }
        JsonDocument template_doc;
        template_doc.set(slots[0]);
        for (int i = static_cast<int>(slots.size()); i < g_state.schedule.num_slots; ++i) {
            JsonObject added = slots.add<JsonObject>();
            added.set(template_doc.as<JsonObject>());
            added["time"] = g_state.schedule.slots[i].time_str;
            added["enabled"] = g_state.schedule.slots[i].enabled;
        }
    }

    const size_t source_size = measureJson(doc);
    if (source_size == 0 || source_size > LAWNBOT_MAX_SCHEDULE_JSON_BYTES) {
        strlcpy(error, "UPDATED SCHEDULE IS TOO LARGE", error_size);
        return false;
    }
    updated_source = "";
    updated_source.reserve(source_size + 1U);
    serializeJson(doc, updated_source);
    body = F("{\"schedule\":");
    body.reserve(updated_source.length() + 14U);
    body += updated_source;
    body += '}';
    return true;
}

static bool bounded_json_float(JsonVariant value, float minimum, float maximum,
                               float &output) {
    if (value.isNull() || !value.is<float>()) return false;
    const float parsed = value.as<float>();
    if (!isfinite(parsed) || parsed < minimum || parsed > maximum) return false;
    output = parsed;
    return true;
}

static bool parse_sensors(const char *json, size_t len) {
    if (!json || len == 0 || len > 8192) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json, len);
    if (err != DeserializationError::Ok) {
        Serial.printf("[JSON] Sensor parse failed: %s\n", err.c_str());
        return false;
    }
    bool any_valid = false;
    JsonObject env = doc["environment"];
    if (!env.isNull()) {
        WeatherData parsed = {};
        float tc = 0.0f;
        parsed.temp_valid = bounded_json_float(env["temperature_c"], -80.0f, 80.0f, tc);
        if (parsed.temp_valid)
            parsed.temp_f = tc * 9.0f / 5.0f + 32.0f;
        parsed.humidity_valid = bounded_json_float(env["humidity_percent"], 0.0f, 100.0f,
                                                   parsed.humidity_pct);
        float wind_ms = 0.0f;
        parsed.wind_speed_valid = bounded_json_float(env["wind_speed_ms"], 0.0f, 150.0f,
                                                     wind_ms);
        if (parsed.wind_speed_valid) parsed.wind_mph = wind_ms * 2.23694f;
        const char *wind_dir = env["wind_direction_compass"] | "";
        parsed.wind_direction_valid = wind_dir[0] && strcmp(wind_dir, "--") != 0 &&
                                      strlen(wind_dir) < sizeof(parsed.wind_dir);
        if (parsed.wind_direction_valid)
            strlcpy(parsed.wind_dir, wind_dir, sizeof(parsed.wind_dir));
        parsed.valid = parsed.temp_valid || parsed.humidity_valid ||
                       parsed.wind_speed_valid || parsed.wind_direction_valid;
        if (parsed.valid) {
            g_state.weather = parsed;
            g_state.weather_updated_ms = millis();
            any_valid = true;
        }
    }

    JsonObject flow = doc["flow_pressure"];
    if (!flow.isNull()) {
        IrrigationSensorData irrigation = {};
        irrigation.flow_valid = bounded_json_float(flow["flow_rate_lpm"], 0.0f, 1000.0f,
                                                    irrigation.flow_rate_lpm);
        irrigation.pressure_valid = bounded_json_float(flow["pressure_psi"], -5.0f, 300.0f,
                                                        irrigation.pressure_psi);
        irrigation.soil_valid = bounded_json_float(flow["soil_moisture_percent"], 0.0f, 100.0f,
                                                    irrigation.soil_moisture_percent);
        irrigation.rainfall_valid = bounded_json_float(flow["rainfall_mm"], 0.0f, 10000.0f,
                                                        irrigation.rainfall_mm);
        irrigation.rainfall_rate_valid = bounded_json_float(flow["rainfall_rate_mm_hr"],
                                                             0.0f, 1000.0f,
                                                             irrigation.rainfall_rate_mm_hr);
        irrigation.valid = irrigation.flow_valid || irrigation.pressure_valid ||
                           irrigation.soil_valid || irrigation.rainfall_valid ||
                           irrigation.rainfall_rate_valid;
        if (irrigation.valid) {
            irrigation.updated_ms = millis();
            g_state.irrigation = irrigation;
            any_valid = true;
        }
    }
    return any_valid;
}

static uint32_t action_mask(PendingType type) {
    return type > PENDING_NONE && type < 32 ? (1UL << static_cast<unsigned>(type)) : 0;
}

static bool action_pending(PendingType type) {
    return (g_pending_action_mask & action_mask(type)) != 0;
}

static void update_data_loading() {
    g_state.data_loading = action_pending(PENDING_FETCH_SCHEDULE) ||
                           action_pending(PENDING_FETCH_HISTORY);
}

static bool send_network_request(NetRequest &request, bool priority) {
    if (!g_net_request_queue) return false;
    const BaseType_t sent = priority
        ? xQueueSendToFront(g_net_request_queue, &request, 0)
        : xQueueSendToBack(g_net_request_queue, &request, 0);
    return sent == pdTRUE;
}

static void finish_reported_action(PendingType type, bool report, bool success,
                                   int code, const char *message) {
    if (!report) return;
    /* STOP may supersede an earlier queued action.  Do not let the older result
     * erase the more safety-critical STOP status. */
    if (g_state.action.busy && g_state.action.type != type) return;
    finish_action(type, success, code, message);
}

static bool enqueue_live_action(const PendingAction &pending, bool report_action) {
    if (!g_net_request_queue || !g_net_result_queue) {
        finish_reported_action(pending.type, report_action, false, 0,
                               "NETWORK WORKER UNAVAILABLE");
        return false;
    }
    if (action_pending(pending.type)) {
        if (report_action) ui_show_toast("REQUEST ALREADY IN PROGRESS", 2500);
        return false;
    }
    if (report_action && g_state.action.busy && pending.type != PENDING_STOP_ALL) {
        ui_show_toast("ANOTHER REQUEST IS IN PROGRESS", 2500);
        return false;
    }

    NetRequest request = {};
    request.action = pending.type;
    request.report_action = report_action;
    strlcpy(request.zone_name, pending.zone_name, sizeof(request.zone_name));
    request.run_minutes = pending.run_minutes;

    const char *progress = "WORKING";
    bool priority = false;
    switch (pending.type) {
        case PENDING_RUN_ZONE:
            if (!bearer_token_configured()) {
                g_state.controls_authenticated = false;
                finish_reported_action(pending.type, report_action, false, 0,
                                       "CONTROL TOKEN NOT CONFIGURED");
                return false;
            }
            if (!pending.zone_name[0]) {
                finish_reported_action(pending.type, report_action, false, 0,
                                       "ZONE NAME IS MISSING");
                return false;
            }
            request.run_minutes = pending.run_minutes > 0
                                ? pending.run_minutes : DEFAULT_RUN_MINUTES;
            if (request.run_minutes < 1 || request.run_minutes > 180) {
                finish_reported_action(pending.type, report_action, false, 0,
                                       "RUN DURATION MUST BE 1-180 MIN");
                return false;
            }
            request.op = NET_RUN_ZONE;
            progress = "STARTING ZONE";
            priority = true;
            break;

        case PENDING_STOP_ALL:
            if (!bearer_token_configured()) {
                g_state.controls_authenticated = false;
                finish_reported_action(pending.type, report_action, false, 0,
                                       "CONTROL TOKEN NOT CONFIGURED");
                return false;
            }
            request.op = NET_STOP_ALL;
            progress = "STOPPING ALL ZONES";
            priority = true;
            break;

        case PENDING_FETCH_SCHEDULE:
            request.op = NET_GET_SCHEDULE;
            progress = "LOADING SCHEDULE";
            break;

        case PENDING_FETCH_HISTORY:
            request.op = NET_GET_HISTORY;
            progress = "LOADING HISTORY";
            break;

        case PENDING_SAVE_SCHEDULE: {
            if (!bearer_token_configured()) {
                g_state.controls_authenticated = false;
                finish_reported_action(pending.type, report_action, false, 0,
                                       "CONTROL TOKEN NOT CONFIGURED");
                return false;
            }
            char error[96] = {};
            if (!build_schedule_put_body(g_pending_save_body, g_pending_save_source,
                                         error, sizeof(error))) {
                finish_reported_action(pending.type, report_action, false, 0, error);
                return false;
            }
            request.op = NET_SAVE_PREFLIGHT;
            progress = "CHECKING SCHEDULE";
            break;
        }

        default:
            return false;
    }

    if (!send_network_request(request, priority)) {
        if (pending.type == PENDING_SAVE_SCHEDULE) {
            g_pending_save_body = "";
            g_pending_save_source = "";
        }
        finish_reported_action(pending.type, report_action, false, 0,
                               "NETWORK QUEUE IS FULL");
        return false;
    }

    if (pending.type == PENDING_STOP_ALL && action_pending(PENDING_RUN_ZONE))
        g_cancelled_action_mask |= action_mask(PENDING_RUN_ZONE);
    g_pending_action_mask |= action_mask(pending.type);
    update_data_loading();
    if (report_action) begin_action(pending.type, progress);
    return true;
}

static bool enqueue_sensor_request() {
    if (g_sensor_request_pending || !g_net_request_queue) return false;
    NetRequest request = {};
    request.op = NET_GET_SENSORS;
    if (!send_network_request(request, false)) return false;
    g_sensor_request_pending = true;
    return true;
}

static int result_error_code(const NetResult &result) {
    if (result.error == NET_RESULT_OK) return result.http_code;
    return -1000 - static_cast<int>(result.error);
}

static const char *result_error_text(const NetResult &result) {
    switch (result.error) {
        case NET_RESULT_WIFI_DOWN:      return "WIFI DISCONNECTED";
        case NET_RESULT_BODY_TOO_LARGE: return "RESPONSE TOO LARGE";
        case NET_RESULT_OUT_OF_MEMORY:  return "NETWORK MEMORY EXHAUSTED";
        case NET_RESULT_READ_FAILED:    return "RESPONSE READ FAILED";
        case NET_RESULT_CANCELLED_BY_STOP: return "CANCELLED BY STOP";
        default:                        return "REQUEST FAILED";
    }
}

static void finish_network_failure(const NetResult &result, const char *operation) {
    char message[96];
    if (result.error != NET_RESULT_OK) {
        snprintf(message, sizeof(message), "%s: %s", operation, result_error_text(result));
    } else if (result.http_code < 0) {
        snprintf(message, sizeof(message), "%s: %s", operation,
                 HTTPClient::errorToString(result.http_code).c_str());
    } else {
        snprintf(message, sizeof(message), "%s: HTTP %d", operation, result.http_code);
    }
    finish_reported_action(result.action, result.report_action, false,
                           result_error_code(result), message);
}

static bool network_result_succeeded(const NetResult &result) {
    return result.error == NET_RESULT_OK && http_success(result.http_code);
}

static void handle_network_result(NetResult &result) {
    bool final_result = true;

    switch (result.op) {
        case NET_GET_SENSORS:
            g_sensor_request_pending = false;
            if (network_result_succeeded(result))
                parse_sensors(result.body, result.body_len);
            break;

        case NET_GET_SCHEDULE:
            if (!network_result_succeeded(result))
                finish_network_failure(result, "SCHEDULE LOAD FAILED");
            else if (parse_schedule(result.body, result.body_len))
                finish_reported_action(result.action, result.report_action, true,
                                       result.http_code, "SCHEDULE LOADED");
            else
                finish_reported_action(result.action, result.report_action, false,
                                       result.http_code, "INVALID SCHEDULE RESPONSE");
            break;

        case NET_GET_HISTORY:
            if (!network_result_succeeded(result))
                finish_network_failure(result, "HISTORY LOAD FAILED");
            else if (parse_history(result.body, result.body_len))
                finish_reported_action(result.action, result.report_action, true,
                                       result.http_code, "HISTORY LOADED");
            else
                finish_reported_action(result.action, result.report_action, false,
                                       result.http_code, "INVALID HISTORY RESPONSE");
            break;

        case NET_RUN_ZONE:
        case NET_STOP_ALL: {
            if (result.op == NET_RUN_ZONE &&
                result.error == NET_RESULT_CANCELLED_BY_STOP) {
                /* STOP intentionally superseded this queued start.  Its own
                 * result remains the user-visible action status. */
                break;
            }
            const bool ok = network_result_succeeded(result);
            if (ok) g_state.controls_authenticated = true;
            else if (result.http_code == 401 || result.http_code == 403 ||
                     result.error == NET_RESULT_WIFI_DOWN)
                g_state.controls_authenticated = false;
            if (ok) {
                finish_reported_action(result.action, result.report_action, true,
                    result.http_code, result.op == NET_STOP_ALL ? "ALL ZONES STOPPED" : "ZONE STARTED");
            } else {
                finish_network_failure(result,
                    result.op == NET_STOP_ALL ? "STOP FAILED" : "START FAILED");
            }
            break;
        }

        case NET_SAVE_PREFLIGHT: {
            if (!network_result_succeeded(result)) {
                finish_network_failure(result, "SCHEDULE CHECK FAILED");
                g_pending_save_body = "";
                g_pending_save_source = "";
                break;
            }
            String current_source;
            if (!canonical_schedule_source(result.body, result.body_len, current_source)) {
                finish_reported_action(result.action, result.report_action, false,
                                       result.http_code, "INVALID CURRENT SCHEDULE");
                g_pending_save_body = "";
                g_pending_save_source = "";
                break;
            }
            if (current_source != g_schedule_source_json) {
                g_state.schedule.save_supported = false;
                finish_reported_action(result.action, result.report_action, false, 409,
                                       "SCHEDULE CHANGED; REFRESH FIRST");
                g_pending_save_body = "";
                g_pending_save_source = "";
                break;
            }

            NetRequest put = {};
            put.op = NET_PUT_SCHEDULE;
            put.action = result.action;
            put.report_action = result.report_action;
            put.payload_len = g_pending_save_body.length();
            put.payload = static_cast<char *>(malloc(put.payload_len));
            if (!put.payload) {
                finish_reported_action(result.action, result.report_action, false, 0,
                                       "NETWORK MEMORY EXHAUSTED");
                g_pending_save_body = "";
                g_pending_save_source = "";
                break;
            }
            memcpy(put.payload, g_pending_save_body.c_str(), put.payload_len);
            if (!send_network_request(put, false)) {
                free(put.payload);
                finish_reported_action(result.action, result.report_action, false, 0,
                                       "NETWORK QUEUE IS FULL");
                g_pending_save_body = "";
                g_pending_save_source = "";
                break;
            }
            final_result = false;
            if (result.report_action) begin_action(result.action, "SAVING SCHEDULE");
            break;
        }

        case NET_PUT_SCHEDULE:
            if (network_result_succeeded(result)) {
                g_state.controls_authenticated = true;
                g_state.schedule.days_dirty = false;
                if (g_state.schedule.num_slots > g_state.schedule.source_num_slots)
                    g_state.schedule.source_num_slots = static_cast<uint8_t>(g_state.schedule.num_slots);
                g_state.schedule.updated_ms = millis();
                g_schedule_source_json = g_pending_save_source;
                finish_reported_action(result.action, result.report_action, true,
                                       result.http_code, "SCHEDULE SAVED");
            } else {
                if (result.http_code == 401 || result.http_code == 403 ||
                    result.error == NET_RESULT_WIFI_DOWN)
                    g_state.controls_authenticated = false;
                finish_network_failure(result, "SCHEDULE SAVE FAILED");
            }
            g_pending_save_body = "";
            g_pending_save_source = "";
            break;
    }

    if (result.body) {
        free(result.body);
        result.body = nullptr;
    }
    if (final_result && result.action != PENDING_NONE) {
        g_pending_action_mask &= ~action_mask(result.action);
        g_cancelled_action_mask &= ~action_mask(result.action);
        update_data_loading();
    }
}

static void poll_network_results() {
    if (!g_net_result_queue) return;
    NetResult result = {};
    while (xQueueReceive(g_net_result_queue, &result, 0) == pdTRUE)
        handle_network_result(result);
}

static void ws_event(WStype_t type, uint8_t *payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            g_state.ws_connected = false;
            invalidate_live_status();
            break;
        case WStype_CONNECTED:    g_state.ws_connected = true;  break;
        case WStype_TEXT:
            if (!parse_ws_status(reinterpret_cast<const char *>(payload), length))
                Serial.printf("[WS] Ignored invalid/oversized status (%u bytes)\n",
                              static_cast<unsigned>(length));
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
    /* Nine-second initial window keeps boot responsive. The loop retries every
     * 10 seconds and starts OTA/data services when WiFi eventually appears. */
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 18) {
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
    configTzTime(NTP_TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
    if (!g_state.wifi_connected) return;
    struct tm ti = {};
    for (int i = 0; i < 8 && !getLocalTime(&ti, 0); i++) {
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

        /* Prefer the hub's authoritative index while its status is fresh.
         * Local civil-date fallback is only for startup/offline display. */
        time_t now; time(&now);
        if (g_state.status_updated_ms == 0 ||
            millis() - g_state.status_updated_ms > LAWNBOT_STATUS_STALE_MS) {
            g_state.schedule.today_idx = calc_today_sched_idx(now);
        }
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
    g_state.weather.temp_valid   = true;
    g_state.weather.humidity_valid = true;
    g_state.weather.wind_speed_valid = true;
    g_state.weather.wind_direction_valid = true;
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
    g_state.history.updated_ms = millis();

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
#if ENABLE_SCREENSHOT_SD
    if (screenshot_sd_init()) {
        Serial.println("[SD] TF card ready");
        Serial0.println("[SD] TF card ready");
    } else {
        Serial.println("[SD] TF card not ready");
        Serial0.println("[SD] TF card not ready");
    }
#endif

#if APP_MODE == APP_MODE_LIVE
    g_state.demo_mode = false;
    g_state.controls_auth_configured = bearer_token_configured();
    g_state.controls_authenticated = false;
    ui_set_splash_text("Connecting to LawnBot...");
    connect_wifi();
    lv_timer_handler();
    sync_ntp();
    lv_timer_handler();
    if (!network_worker_init())
        Serial.println("[HTTP] Failed to start network worker");

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

    /* Data arrives asynchronously after the dashboard is already responsive. */
    PendingAction initial_schedule = {PENDING_FETCH_SCHEDULE, "", 0};
    PendingAction initial_history = {PENDING_FETCH_HISTORY, "", 0};
    enqueue_live_action(initial_schedule, false);
    enqueue_live_action(initial_history, false);
    enqueue_sensor_request();
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
    poll_network_results();

    /* Periodic sensor poll every 15 s */
    uint32_t now = millis();
    if (now - g_last_sensor_ms > 15000UL) {
        g_last_sensor_ms = now;
        enqueue_sensor_request();
    }

    if (g_state.status_updated_ms != 0 &&
        now - g_state.status_updated_ms > LAWNBOT_STATUS_STALE_MS) {
        Serial.println("[WS] Status expired; invalidating actuator state");
        invalidate_live_status();
    }
    if (g_state.weather_updated_ms != 0 &&
        now - g_state.weather_updated_ms > LAWNBOT_WEATHER_STALE_MS) {
        g_state.weather.valid = false;
        g_state.weather.temp_valid = false;
        g_state.weather.humidity_valid = false;
        g_state.weather.wind_speed_valid = false;
        g_state.weather.wind_direction_valid = false;
    }
    if (g_state.irrigation.updated_ms != 0 &&
        now - g_state.irrigation.updated_ms > LAWNBOT_WEATHER_STALE_MS) {
        g_state.irrigation.valid = false;
    }

    /* WiFi reconnect check */
    static uint32_t last_wifi_ms = 0;
    if (now - last_wifi_ms > 10000UL) {
        last_wifi_ms = now;
        bool ok = (WiFi.status() == WL_CONNECTED);
        const bool was_connected = g_state.wifi_connected;
        if (ok != g_state.wifi_connected) {
            g_state.wifi_connected = ok;
            if (!ok) {
                g_state.ws_connected = false;
                g_state.controls_authenticated = false;
                g_state.weather.valid = false;
                g_state.weather.temp_valid = false;
                g_state.weather.humidity_valid = false;
                g_state.weather.wind_speed_valid = false;
                g_state.weather.wind_direction_valid = false;
                g_state.irrigation.valid = false;
                invalidate_live_status();
            }
        }
        if (!ok) WiFi.reconnect();
        else if (!was_connected) {
            if (!g_state.schedule.valid) {
                PendingAction request = {PENDING_FETCH_SCHEDULE, "", 0};
                enqueue_live_action(request, false);
            }
            if (!g_state.history.valid) {
                PendingAction request = {PENDING_FETCH_HISTORY, "", 0};
                enqueue_live_action(request, false);
            }
            enqueue_sensor_request();
        }
#if ENABLE_OTA
        /* If WiFi came up after the boot-time init missed the window,
         * bring OTA online now (ota_server_init is idempotent). */
        if (g_state.wifi_connected) ota_server_init();
#endif
    }
#else
    update_demo_state();
#endif

    g_state.controls_visible = (millis() - g_last_touch_ms) < CTRL_WAKE_MS;
    lv_timer_handler();
#if ENABLE_SCREENSHOT_SD
    poll_screenshot_sd_result();
#endif

    if (g_pending.type != PENDING_NONE) {
        if (g_pending.type == PENDING_SAVE_SCREENSHOT_SD) {
#if ENABLE_SCREENSHOT_SD
            start_screenshot_sd_request();
#else
            g_pending.type = PENDING_NONE;
            ui_set_snap_busy(false);
            ui_show_toast("SD SCREENSHOT DISABLED", 2500);
#endif
        } else {
#if APP_MODE == APP_MODE_LIVE
            PendingAction request = {};
            request.type = g_pending.type;
            strlcpy(request.zone_name, g_pending.zone_name, sizeof(request.zone_name));
            request.run_minutes = g_pending.run_minutes;
            g_pending.type = PENDING_NONE;
            enqueue_live_action(request, true);
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
