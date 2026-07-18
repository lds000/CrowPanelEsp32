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

/* Device/network secrets belong in the ignored config_private.h.  This file
 * intentionally contains only safe defaults so an enthusiastic `git add -A`
 * does not publish the irrigation credentials to the entire planet. */
#if !defined(LAWNBOT_SKIP_PRIVATE_CONFIG) && __has_include("config_private.h")
#include "config_private.h"
#endif

#ifndef APP_MODE
#define APP_MODE       APP_MODE_DEMO
#endif

/* ── WiFi ─────────────────────────────────────────────────── */
#ifndef WIFI_SSID
#define WIFI_SSID      ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD  ""
#endif

/* ── LawnBot Controller Pi ────────────────────────────────── */
#ifndef LAWNBOT_HOST
#define LAWNBOT_HOST   "192.168.1.100"   /* Set in config_private.h for live mode */
#endif
#define LAWNBOT_PORT   8000
#define LAWNBOT_WS_PATH   "/ws"
#define LAWNBOT_API_BASE  "http://" LAWNBOT_HOST ":8000/api"

/* Bearer credential used only for mutating controller requests.  Keep the real
 * value out of Git: provide -DLAWNBOT_API_BEARER_TOKEN=\"...\" from a private
 * build environment or an ignored local header.  Empty means controls fail
 * closed while read-only status remains available. */
#ifndef LAWNBOT_API_BEARER_TOKEN
#define LAWNBOT_API_BEARER_TOKEN ""
#endif

/* Defensive limits and freshness windows. */
#define LAWNBOT_MAX_WS_JSON_BYTES       (16U * 1024U)
#define LAWNBOT_MAX_SCHEDULE_JSON_BYTES (24U * 1024U)
#define LAWNBOT_MAX_HISTORY_JSON_BYTES  (24U * 1024U)
#define LAWNBOT_STATUS_STALE_MS          30000UL
#define LAWNBOT_WEATHER_STALE_MS         45000UL

/* ── Defaults ──────────────────────────────────────────────── */
#define DEFAULT_RUN_MINUTES  5    /* Duration when tapping RUN on a zone card */

/* ── Demo mode ─────────────────────────────────────────────── */
#define DEMO_AUTOCYCLE_SECONDS   20   /* Idle time before auto-starting a demo run */
#define DEMO_AUTORUN_SECONDS     75   /* Hands-free animated demo run length */
#define DEMO_START_EPOCH         1711029720UL  /* Thu Mar 21 2024 07:22:00 local-ish demo seed */

/* ── NTP ───────────────────────────────────────────────────── */
#define NTP_SERVER_1  "pool.ntp.org"
#define NTP_SERVER_2  "time.google.com"
/* Boise, ID / US Mountain with automatic DST transitions */
#define NTP_TZ_INFO   "MST7MDT,M3.2.0/2,M11.1.0/2"

/* ── OTA update server ─────────────────────────────────────────
 * ArduinoOTA (PlatformIO espota) + HTTP browser upload page.
 * Active only in LIVE mode after WiFi connects.
 *   Browser : http://<device-ip>/
 *   PlatformIO : use env:crowpanel-7inch-ota with CROWPANEL_OTA_HOST set
 * Change OTA_PASSWORD before deploying to a shared network. */
#define ENABLE_OTA      1
#define OTA_HOSTNAME    "crowpanel"
#define OTA_USERNAME    "admin"
#ifndef OTA_PASSWORD
#define OTA_PASSWORD    "change-me"
#endif
#define OTA_HTTP_PORT   80

/* ── Debug: HTTP screen capture (LVGL snapshot → BMP) ─────────
 * When 1, connects to WIFI_SSID and serves:
 *   http://<device-ip>:8080/capture.bmp
 *   http://<device-ip>:8080/          (links to capture)
 * Set to 0 to save flash / avoid WiFi in demo. */
/* WiFi HTTP capture — set 1 at home; leave 0 on locked work WiFi (saves boot time). */
#ifndef ENABLE_SCREENSHOT_HTTP
#define ENABLE_SCREENSHOT_HTTP   0
#endif
#ifndef SCREENSHOT_HTTP_PORT
#define SCREENSHOT_HTTP_PORT     8080
#endif
#ifndef SCREENSHOT_HTTP_AUTH_USERNAME
#define SCREENSHOT_HTTP_AUTH_USERNAME "crowpanel"
#endif
#ifndef SCREENSHOT_HTTP_AUTH_TOKEN
#define SCREENSHOT_HTTP_AUTH_TOKEN ""
#endif

/* SD screenshot saving currently has no UI trigger.  Keep the entire SD/SPI
 * path out of production builds until it is intentionally exposed. */
#ifndef ENABLE_SCREENSHOT_SD
#define ENABLE_SCREENSHOT_SD     0
#endif
