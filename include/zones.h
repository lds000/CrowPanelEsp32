#pragma once
/**
 * zones.h - Canonical zone names and lookup helpers.
 *
 * API names must match the hub's FastAPI route paths exactly
 * (URL-encoding is handled separately by zone_url_path()).
 */

#include <string.h>

#define ZONE_COUNT 3

static const char * const ZONE_API_NAMES[ZONE_COUNT]     = {"Hanging Pots", "Garden", "Misters"};
static const char * const ZONE_DISPLAY_NAMES[ZONE_COUNT] = {"HANGING POTS", "GARDEN",  "MISTERS"};

/* Index of an API zone name, or -1 if unknown */
static inline int zone_index(const char *api) {
    for (int i = 0; i < ZONE_COUNT; i++)
        if (strcmp(api, ZONE_API_NAMES[i]) == 0) return i;
    return -1;
}

static inline const char *zone_display_name(const char *api) {
    int i = zone_index(api);
    return i >= 0 ? ZONE_DISPLAY_NAMES[i] : api;
}

/* URL-encode spaces in zone names for HTTP paths */
static inline const char *zone_url_path(const char *api) {
    if (strcmp(api, "Hanging Pots") == 0) return "Hanging%20Pots";
    return api;
}
