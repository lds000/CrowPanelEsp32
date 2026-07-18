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
static bool g_ota_started = false;

static bool ota_password_configured() {
    String password = OTA_PASSWORD;
    password.trim();
    password.toLowerCase();
    return !password.isEmpty() && password != "change-me" && password != "placeholder" &&
           password.indexOf("placeholder") < 0 && password.indexOf("replace-with") < 0;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ota_server_init() {
    /* Idempotency guard — loop() re-invokes this on every WiFi reconnect;
     * re-running ArduinoOTA.begin() / ElegantOTA.begin() / g_ota_http.begin()
     * can leak handlers or assert on some Arduino-ESP32 versions.
     * (Originally added in ffd422e, restored after ElegantOTA migration.) */
    static bool s_init_attempted = false;
    if (s_init_attempted) return;
    s_init_attempted = true;

    /* Reject committed defaults and documentation sentinels before either
     * ArduinoOTA or the HTTP upload server is configured or started. */
    if (!ota_password_configured()) {
        Serial.println("[OTA] Disabled: configure a private OTA password");
        return;
    }

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

    /* ElegantOTA 2.x browser upload at /update. Version 3 is substantially
       smaller but changes APIs and uses AGPL-3.0 for the open-source edition;
       keep that migration isolated until compatibility and licensing have
       been reviewed. */
    ElegantOTA.begin(&g_ota_http, OTA_USERNAME, OTA_PASSWORD);
    g_ota_http.begin();
    g_ota_started = true;

    Serial.printf("[OTA] ArduinoOTA  — %s.local  (port 3232)\n", OTA_HOSTNAME);
    Serial.printf("[OTA] Browser OTA — http://%s/update\n",
                  WiFi.localIP().toString().c_str());
}

void ota_server_loop() {
    if (!g_ota_started) return;
    ArduinoOTA.handle();
    g_ota_http.handleClient();
    /* ElegantOTA.loop() does not exist in 2.2.9 — the WebServer poll above
     * is sufficient because the upload is handled inside the request. */
}

#endif /* APP_MODE_LIVE && ENABLE_OTA */

/* ── DEMO mode stubs (no WiFi, nothing to compile) ──────────────────── */
#if !(APP_MODE == APP_MODE_LIVE && ENABLE_OTA)
void ota_server_init() {}
void ota_server_loop() {}
#endif
