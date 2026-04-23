/**
 * OTA update server — ArduinoOTA (PlatformIO espota) + HTTP browser upload.
 *
 * ArduinoOTA  → PlatformIO flashes over WiFi: upload_protocol = espota
 * HTTP server → browser visits http://<device-ip>/ to upload a .bin file
 *
 * Active in LIVE mode only (compiled out otherwise).
 */

#include "config.h"

#if APP_MODE == APP_MODE_LIVE && ENABLE_OTA

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>

#include "ota_server.h"

/* Defined in main.cpp / ui.cpp */
extern void ui_show_toast(const char *text, uint32_t ms);

static WebServer g_ota_http(OTA_HTTP_PORT);
static bool      g_ota_in_progress = false;

/* ── HTML page ───────────────────────────────────────────────────────── */

static void handle_root() {
    char page[3072];
    snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>CrowPanel OTA</title>"
        "<style>"
        "body{background:#1a1a2e;color:#e0e0e0;font-family:sans-serif;"
            "max-width:480px;margin:40px auto;padding:16px}"
        "h1{color:#00bcd4;margin-bottom:4px}"
        "h2{color:#888;font-size:.9em;font-weight:normal;margin:0 0 24px}"
        ".card{background:#16213e;border:1px solid #0f3460;border-radius:8px;"
            "padding:16px;margin-bottom:16px}"
        ".row{display:flex;justify-content:space-between;padding:4px 0;"
            "border-bottom:1px solid #0f3460}"
        ".row:last-child{border:none}"
        ".label{color:#888}"
        ".val{color:#e0e0e0;font-family:monospace}"
        "input[type=file]{width:100%%;padding:8px;background:#0f3460;"
            "border:1px solid #00bcd4;border-radius:4px;color:#e0e0e0;"
            "cursor:pointer;margin-bottom:12px;box-sizing:border-box}"
        "button{background:#00bcd4;color:#000;border:none;padding:10px 24px;"
            "border-radius:4px;cursor:pointer;font-size:1em;width:100%%}"
        "button:hover{background:#0097a7}"
        "#status{margin-top:12px;padding:10px;border-radius:4px;display:none;"
            "text-align:center}"
        ".ok{background:#1b5e20;color:#a5d6a7}"
        ".err{background:#b71c1c;color:#ffcdd2}"
        "</style></head><body>"
        "<h1>CrowPanel OTA</h1>"
        "<h2>LawnBot Display — Firmware Update</h2>"
        "<div class='card'>"
        "<div class='row'><span class='label'>Hostname</span>"
            "<span class='val'>%s.local</span></div>"
        "<div class='row'><span class='label'>IP Address</span>"
            "<span class='val'>%s</span></div>"
        "<div class='row'><span class='label'>Build</span>"
            "<span class='val'>%s %s</span></div>"
        "<div class='row'><span class='label'>Free Heap</span>"
            "<span class='val'>%u KB</span></div>"
        "<div class='row'><span class='label'>Uptime</span>"
            "<span class='val'>%lu s</span></div>"
        "</div>"
        "<div class='card'>"
        "<form method='POST' action='/update' enctype='multipart/form-data' id='frm'>"
        "<input type='file' name='firmware' accept='.bin' required>"
        "<button type='submit'>Upload Firmware</button>"
        "</form>"
        "<div id='status'></div>"
        "</div>"
        "<script>"
        "document.getElementById('frm').addEventListener('submit',function(){"
        "  var s=document.getElementById('status');"
        "  s.className='ok';s.style.display='block';"
        "  s.textContent='Uploading\u2026 do not close this page.';"
        "});"
        "</script>"
        "</body></html>",
        OTA_HOSTNAME,
        WiFi.localIP().toString().c_str(),
        __DATE__, __TIME__,
        (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned long)(millis() / 1000)
    );
    g_ota_http.send(200, "text/html", page);
}

static void handle_update_upload() {
    HTTPUpload &up = g_ota_http.upload();
    if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA-HTTP] Receiving: %s\n", up.filename.c_str());
        ui_show_toast("OTA UPLOADING...", 60000);
        g_ota_in_progress = true;
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
            Update.printError(Serial);
        }
    } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA-HTTP] Done: %u bytes\n", up.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

static void handle_update_finish() {
    if (Update.hasError()) {
        g_ota_http.send(500, "text/html",
            "<html><body style='background:#1a1a2e;color:#ff5252;"
            "font-family:sans-serif;text-align:center;padding:40px'>"
            "<h1>Update Failed</h1>"
            "<p>Check serial output for details.</p>"
            "<p><a href='/' style='color:#00bcd4'>Back</a></p>"
            "</body></html>");
        ui_show_toast("OTA FAILED", 5000);
        g_ota_in_progress = false;
    } else {
        g_ota_http.send(200, "text/html",
            "<html><body style='background:#1a1a2e;color:#a5d6a7;"
            "font-family:sans-serif;text-align:center;padding:40px'>"
            "<h1>Update Successful</h1>"
            "<p>Rebooting in 3 seconds\u2026</p>"
            "</body></html>");
        ui_show_toast("OTA SUCCESS — REBOOTING", 5000);
        delay(3000);
        ESP.restart();
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ota_server_init() {
    /* Idempotent — calling this multiple times (e.g. after a late WiFi
     * reconnect) is a no-op after the first successful invocation. */
    static bool s_initialized = false;
    if (s_initialized) return;
    s_initialized = true;

    /* ArduinoOTA — used by PlatformIO espota upload */
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
        Serial.printf("[OTA] Start: %s\n", type.c_str());
        ui_show_toast("OTA IN PROGRESS...", 120000);
    });
    ArduinoOTA.onEnd([]() {
        Serial.println("[OTA] Complete");
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

    /* HTTP browser upload server */
    g_ota_http.on("/",       HTTP_GET,  handle_root);
    g_ota_http.on("/update", HTTP_POST, handle_update_finish, handle_update_upload);
    g_ota_http.begin();

    Serial.printf("[OTA] ArduinoOTA  — %s.local  (port 3232)\n", OTA_HOSTNAME);
    Serial.printf("[OTA] Browser OTA — http://%s/\n",
                  WiFi.localIP().toString().c_str());
}

void ota_server_loop() {
    ArduinoOTA.handle();
    if (!g_ota_in_progress) g_ota_http.handleClient();
}

#else /* not (APP_MODE_LIVE && ENABLE_OTA) */

/* Provide empty stubs so unconditional callers in main.cpp still link
 * when OTA is compiled out (e.g. in DEMO mode). */
#include "ota_server.h"
void ota_server_init() {}
void ota_server_loop() {}

#endif /* APP_MODE_LIVE && ENABLE_OTA */
