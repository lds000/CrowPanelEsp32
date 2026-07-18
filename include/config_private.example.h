#pragma once

/* Copy to config_private.h (which is ignored by Git) and fill in locally. */
#define APP_MODE       APP_MODE_LIVE
#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"
#define LAWNBOT_HOST   "192.168.1.100"
#define OTA_PASSWORD   "replace-with-a-long-random-password"

/* Use a separate credential for the optional diagnostic screenshot server. */
#define ENABLE_SCREENSHOT_HTTP       1
#define SCREENSHOT_HTTP_AUTH_USERNAME "crowpanel"
#define SCREENSHOT_HTTP_AUTH_TOKEN    "replace-with-a-different-random-token"

/* Required by authenticated run/stop/schedule-update endpoints. */
#define LAWNBOT_API_BEARER_TOKEN "replace-with-device-scoped-token"
