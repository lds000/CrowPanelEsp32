#pragma once
/**
 * ui_theme.h — Shared color/style constants for all UI files.
 */

/* ── Colors ───────────────────────────────────────────────── */
#define C_TEXT        0xF8FBFFu
#define C_MUTED       0xC5D2E3u
#define C_ORANGE      0xF16322u
#define C_BLUE        0x0033A0u
#define C_DANGER      0xD64541u
#define C_SUCCESS     0x3BA84Au
#define C_PANEL       0x102445u
#define C_PANEL_EDGE  0x2E507Cu
#define C_IDLE_MASK   0x06182Fu
#define C_DAY_ON      0x1A5CB8u   /* schedule day: active */
#define C_DAY_OFF     0x182035u   /* schedule day: inactive */

#define SCREEN_W 800
#define SCREEN_H 480

/* Zone names — must match FastAPI route paths (URL-encoded separately) */
static const char * const ZONE_API_NAMES[3]     = {"Hanging Pots", "Garden", "Misters"};
static const char * const ZONE_DISPLAY_NAMES[3] = {"HANGING POTS", "GARDEN",  "MISTERS"};

static inline const char *zone_display_name(const char *api) {
    for (int i = 0; i < 3; i++)
        if (strcmp(api, ZONE_API_NAMES[i]) == 0) return ZONE_DISPLAY_NAMES[i];
    return api;
}

/* URL-encode spaces in zone names for HTTP paths */
static inline const char *zone_url_path(const char *api) {
    if (strcmp(api, "Hanging Pots") == 0) return "Hanging%20Pots";
    return api;
}
