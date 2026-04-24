#pragma once

/**
 * LawnBot Display — User Configuration
 *
 * Start in DEMO mode to build and use the UI anywhere.
 * Switch to LIVE mode later when the CrowPanel is on the same local network
 * as the LawnBot Controller Pi.
 */

/* ── Application mode ─────────────────────────────────────── */
#define APP_MODE_DEMO  1
#define APP_MODE_LIVE  2
#define APP_MODE       APP_MODE_DEMO

/* ── WiFi ─────────────────────────────────────────────────── */
#define WIFI_SSID      "Guest"
#define WIFI_PASSWORD  "Healthy!"

/* ── LawnBot Controller Pi ────────────────────────────────── */
#define LAWNBOT_HOST   "192.168.1.100"   /* Set to local Pi host/IP when APP_MODE_LIVE */
#define LAWNBOT_PORT   8000
#define LAWNBOT_WS_PATH   "/ws"
#define LAWNBOT_API_BASE  "http://" LAWNBOT_HOST ":8000/api"

/* ── Defaults ──────────────────────────────────────────────── */
#define DEFAULT_RUN_MINUTES  5    /* Duration when tapping RUN on a zone card */

/* Temporary UI workbench: boot directly into the controls view and hide the
 * idle/active dashboard content while refining the controls. */
#define UI_CONTROLS_ONLY_DEBUG  1

/* ── Demo mode ─────────────────────────────────────────────── */
#define DEMO_AUTOCYCLE_SECONDS   20   /* Idle time before auto-starting a demo run */
#define DEMO_AUTORUN_SECONDS     75   /* Hands-free animated demo run length */
#define DEMO_START_EPOCH         1711029720UL  /* Thu Mar 21 2024 07:22:00 local-ish demo seed */

/* ── NTP ───────────────────────────────────────────────────── */
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.google.com"
#define NTP_GMT_OFFSET_SEC   (-6 * 3600)   /* UTC-6 (CST) — adjust for your timezone */
#define NTP_DAYLIGHT_OFFSET_SEC  3600       /* +1 hour daylight saving */

/* ── OTA update server ─────────────────────────────────────────
 * ArduinoOTA (PlatformIO espota) + HTTP browser upload page.
 * Active only in LIVE mode after WiFi connects.
 *   Browser : http://<device-ip>/
 *   PlatformIO : use env:crowpanel-7inch-ota  (upload_port = 192.168.68.107)
 * Change OTA_PASSWORD before deploying to a shared network. */
#define ENABLE_OTA      1
#define OTA_HOSTNAME    "crowpanel"
#define OTA_USERNAME    "admin"
#define OTA_PASSWORD    "lawnbot"
#define OTA_HTTP_PORT   80

/* ── Debug: HTTP screen capture (LVGL snapshot → BMP) ─────────
 * When 1, connects to WIFI_SSID and serves:
 *   http://<device-ip>:8080/capture.bmp
 *   http://<device-ip>:8080/          (links to capture)
 * Set to 0 to save flash / avoid WiFi in demo. */
/* WiFi HTTP capture — set 1 at home; leave 0 on locked work WiFi (saves boot time). */
#define ENABLE_SCREENSHOT_HTTP   1
#define SCREENSHOT_HTTP_PORT     8080
