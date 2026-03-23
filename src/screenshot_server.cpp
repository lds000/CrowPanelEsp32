/**
 * HTTP screen capture — optional (needs WiFi).
 */

#include "config.h"

#if ENABLE_SCREENSHOT_HTTP

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <lvgl.h>

#include "screenshot_bmp.h"
#include "screenshot_server.h"

static WebServer *g_http = nullptr;

static void handle_root() {
    String ip = WiFi.localIP().toString();
    String html;
    html.reserve(256);
    html += F("<!DOCTYPE html><html><head><meta charset='utf-8'><title>LawnBot Display</title></head><body>");
    html += F("<h1>LawnBot CrowPanel</h1><p>IP: ");
    html += ip;
    html += F("</p><p><a href=\"/capture.bmp\">Download screen capture (BMP)</a></p>");
    html += F("<p><small>LVGL snapshot of the active screen</small></p></body></html>");
    g_http->send(200, "text/html", html);
}

static void handle_capture() {
    uint8_t *bmp = nullptr;
    uint32_t sz = 0;
    if (!screenshot_bmp_capture(&bmp, &sz)) {
        g_http->send(500, "text/plain", "capture failed");
        return;
    }
    g_http->setContentLength(sz);
    g_http->send(200, "image/bmp", "");
    g_http->sendContent((const char *)bmp, sz);
    screenshot_bmp_free(bmp);
}

void screenshot_server_init() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf("[HTTP] Screenshot server: connecting WiFi (%s)...\n", WIFI_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
            delay(250);
            Serial.print('.');
        }
        Serial.println();
    } else {
        Serial.println("[HTTP] Screenshot server: using existing WiFi connection");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] WiFi failed — /capture.bmp unavailable");
        return;
    }

    Serial.printf("[HTTP] Screenshot: http://%s:%d/capture.bmp\n",
                  WiFi.localIP().toString().c_str(), SCREENSHOT_HTTP_PORT);

    g_http = new WebServer(SCREENSHOT_HTTP_PORT);
    g_http->on("/", HTTP_GET, handle_root);
    g_http->on("/capture.bmp", HTTP_GET, handle_capture);
    g_http->begin();
}

void screenshot_server_loop() {
    if (g_http) g_http->handleClient();
}

#endif /* ENABLE_SCREENSHOT_HTTP */
