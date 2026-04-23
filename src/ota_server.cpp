/**
 * OTA update server — ArduinoOTA (PlatformIO espota) + ElegantOTA browser upload.
 *
 * ArduinoOTA  → PlatformIO flashes over WiFi: upload_protocol = espota
 * ElegantOTA  → browser visits http://<device-ip>/update to upload a .bin file
 *               Uses chunked streaming so large firmware fits in ESP32 heap.
 *
 * Active in LIVE mode only (compiled out otherwise).
 */

#include "config.h"

#if APP_MODE == APP_MODE_LIVE && ENABLE_OTA

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ElegantOTA.h>
#include <WebServer.h>
#include <WiFi.h>

#include "ota_server.h"

/* Defined in ui.cpp */
extern void ui_show_toast(const char *text, uint32_t ms);

static WebServer g_ota_http(OTA_HTTP_PORT);

static void on_ota_start() {
    Serial.println("[OTA] Update starting");
    ui_show_toast("OTA IN PROGRESS...", 120000);
}

static void on_ota_end(bool success) {
    if (success) {
        Serial.println("[OTA] Update complete — rebooting");
        ui_show_toast("OTA DONE — REBOOTING", 4000);
    } else {
        Serial.println("[OTA] Update failed");
        ui_show_toast("OTA FAILED", 5000);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ota_server_init() {
    /* ArduinoOTA — used by PlatformIO espota upload */
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        Serial.printf("[OTA] ArduinoOTA start: %s\n", type.c_str());
        ui_show_toast("OTA IN PROGRESS...", 120000);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] ArduinoOTA complete");
        ui_show_toast("OTA DONE — REBOOTING", 4000);
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        Serial.printf("[OTA] %u%%\r", (done * 100) / total);
    });
    ArduinoOTA.onError([](ota_error_t err) {
        Serial.printf("[OTA] Error[%u]\n", err);
        ui_show_toast("OTA ERROR", 5000);
    });
    ArduinoOTA.begin();

    /* ElegantOTA browser upload — handles large .bin files in chunks */
    ElegantOTA.begin(&g_ota_http, OTA_USERNAME, OTA_PASSWORD);
    ElegantOTA.onStart(on_ota_start);
    ElegantOTA.onEnd(on_ota_end);
    g_ota_http.begin();

    Serial.printf("[OTA] ArduinoOTA  — %s.local  (port 3232)\n", OTA_HOSTNAME);
    Serial.printf("[OTA] Browser OTA — http://%s/update\n",
                  WiFi.localIP().toString().c_str());
}

void ota_server_loop() {
    ArduinoOTA.handle();
    g_ota_http.handleClient();
    ElegantOTA.loop();
}

#endif /* APP_MODE_LIVE && ENABLE_OTA */

/* ── DEMO mode stubs (no WiFi, nothing to compile) ──────────────────── */
#if !(APP_MODE == APP_MODE_LIVE && ENABLE_OTA)
void ota_server_init() {}
void ota_server_loop() {}
#endif
