/**
 * HTTP screen capture — optional (needs WiFi).
 */

#include "config.h"

#if ENABLE_SCREENSHOT_HTTP

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <lvgl.h>

#include "app_state.h"
#include "screenshot_bmp.h"
#include "screenshot_server.h"

static WebServer *g_http = nullptr;
static TaskHandle_t g_http_task = nullptr;
static uint32_t g_last_init_attempt_ms = 0;
static uint32_t g_last_capture_ms = 0;

#ifndef SCREENSHOT_HTTP_REQUIRE_AUTH
#define SCREENSHOT_HTTP_REQUIRE_AUTH 1
#endif

#ifndef SCREENSHOT_HTTP_AUTH_USERNAME
#define SCREENSHOT_HTTP_AUTH_USERNAME "crowpanel"
#endif

#ifndef SCREENSHOT_HTTP_AUTH_TOKEN
#define SCREENSHOT_HTTP_AUTH_TOKEN ""
#endif

#ifndef SCREENSHOT_CAPTURE_MIN_INTERVAL_MS
#define SCREENSHOT_CAPTURE_MIN_INTERVAL_MS 5000UL
#endif

#ifndef SCREENSHOT_HTTP_ALLOW_LEGACY_WAKE_GET
#define SCREENSHOT_HTTP_ALLOW_LEGACY_WAKE_GET 0
#endif

static void security_headers() {
    g_http->sendHeader("Cache-Control", "no-store");
    g_http->sendHeader("X-Content-Type-Options", "nosniff");
    g_http->sendHeader("X-Frame-Options", "DENY");
}

static bool authorize_request() {
#if SCREENSHOT_HTTP_REQUIRE_AUTH
    if (SCREENSHOT_HTTP_AUTH_TOKEN[0] == '\0' || strcmp(SCREENSHOT_HTTP_AUTH_TOKEN, "change-me") == 0) {
        security_headers();
        g_http->send(503, "text/plain", "screenshot authentication is not configured\n");
        return false;
    }
    if (!g_http->authenticate(SCREENSHOT_HTTP_AUTH_USERNAME, SCREENSHOT_HTTP_AUTH_TOKEN)) {
        security_headers();
        g_http->requestAuthentication(BASIC_AUTH, "CrowPanel diagnostics");
        return false;
    }
#endif
    if (g_http->header("Sec-Fetch-Site").equalsIgnoreCase("cross-site")) {
        security_headers();
        g_http->send(403, "text/plain", "cross-site request rejected\n");
        return false;
    }
    return true;
}

static void handle_root() {
    if (!authorize_request()) return;
    String ip = WiFi.localIP().toString();
    String html;
    html.reserve(256);
    html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>LawnBot Display</title></head><body>");
    html += F("<h1>LawnBot CrowPanel</h1><p>IP: ");
    html += ip;
    html += F("</p><p><a href=\"/capture.bmp\">Download screen capture (BMP)</a></p>");
    html += F("<button id='wake'>Wake controls</button><span id='wake-result'></span>");
    html += F("<script>document.getElementById('wake').onclick=async()=>{const r=await fetch('/wake-controls',{method:'POST',headers:{'X-CrowPanel-Action':'1'}});document.getElementById('wake-result').textContent=await r.text();}</script>");
    html += F("<p><small>LVGL snapshot of the active screen</small></p></body></html>");
    security_headers();
    g_http->send(200, "text/html", html);
}

static void handle_capture() {
    if (!authorize_request()) return;
    const uint32_t now = millis();
    if (g_last_capture_ms != 0 && now - g_last_capture_ms < SCREENSHOT_CAPTURE_MIN_INTERVAL_MS) {
        uint32_t remaining_ms = SCREENSHOT_CAPTURE_MIN_INTERVAL_MS - (now - g_last_capture_ms);
        g_http->sendHeader("Retry-After", String((remaining_ms + 999) / 1000));
        security_headers();
        g_http->send(429, "text/plain", "capture rate limited\n");
        return;
    }
    g_last_capture_ms = now;
    uint8_t *bmp = nullptr;
    uint32_t sz = 0;
    if (!screenshot_bmp_capture(&bmp, &sz)) {
        security_headers();
        g_http->send(500, "text/plain", "capture failed\n");
        return;
    }
    security_headers();
    g_http->sendHeader("Content-Disposition", "attachment; filename=\"crowpanel.bmp\"");
    g_http->setContentLength(sz);
    g_http->send(200, "image/bmp", "");
    g_http->sendContent((const char *)bmp, sz);
    screenshot_bmp_free(bmp);
}

static void handle_wake_controls() {
    if (!authorize_request()) return;
    if (g_http->header("X-CrowPanel-Action") != "1") {
        security_headers();
        g_http->send(403, "text/plain", "missing action confirmation header\n");
        return;
    }
    g_last_touch_ms = millis();
    security_headers();
    g_http->send(200, "text/plain", "controls awake\n");
}

static void screenshot_http_task(void *) {
    for (;;) {
        if (g_http && WiFi.status() == WL_CONNECTED) {
            /* This task is the sole owner of WebServer request handling. A
             * multi-minute BMP transfer can block here without starving the
             * LVGL/WebSocket/OTA loop on the other core. */
            g_http->handleClient();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void screenshot_server_init() {
    if (g_http || WiFi.status() != WL_CONNECTED) return;

    Serial.printf("[HTTP] Screenshot: http://%s:%d/capture.bmp\n",
                  WiFi.localIP().toString().c_str(), SCREENSHOT_HTTP_PORT);

    g_http = new WebServer(SCREENSHOT_HTTP_PORT);
    const char *header_keys[] = {"X-CrowPanel-Action", "Sec-Fetch-Site"};
    g_http->collectHeaders(header_keys, 2);
    g_http->on("/", HTTP_GET, handle_root);
    g_http->on("/capture.bmp", HTTP_GET, handle_capture);
    g_http->on("/wake-controls", HTTP_POST, handle_wake_controls);
#if SCREENSHOT_HTTP_ALLOW_LEGACY_WAKE_GET
    g_http->on("/wake-controls", HTTP_GET, handle_wake_controls);
#endif
    g_http->onNotFound([]() {
        security_headers();
        g_http->send(404, "text/plain", "not found\n");
    });
    g_http->begin();
    BaseType_t task_ok = xTaskCreatePinnedToCore(
        screenshot_http_task,
        "screenshot-http",
        6144,
        nullptr,
        1,
        &g_http_task,
        0);
    if (task_ok != pdPASS) {
        Serial.println("[HTTP] Screenshot task failed to start");
        g_http->stop();
        delete g_http;
        g_http = nullptr;
        g_http_task = nullptr;
    }
}

void screenshot_server_loop() {
    if (!g_http) {
        const uint32_t now = millis();
        if (WiFi.status() == WL_CONNECTED &&
            (g_last_init_attempt_ms == 0 || now - g_last_init_attempt_ms >= 5000UL)) {
            g_last_init_attempt_ms = now;
            screenshot_server_init();
        }
        return;
    }
    /* WebServer is serviced exclusively by screenshot_http_task(). */
}

#endif /* ENABLE_SCREENSHOT_HTTP */
