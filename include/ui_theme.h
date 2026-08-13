#pragma once
/**
 * ui_theme.h - Shared color/style constants for all UI files.
 *
 * Single "Boise State Edition" palette: deep blue panels, orange accent.
 * Zone name helpers live in zones.h.
 */

/* Base */
#define C_TEXT        0xF8FBFFu
#define C_MUTED       0xC5D2E3u
#define C_ORANGE      0xF16322u   /* brand accent */
#define C_BLUE        0x0033A0u   /* brand blue */

/* Semantic */
#define C_DANGER      0xD64541u
#define C_DANGER_DARK 0xA83430u   /* danger button resting state */
#define C_SUCCESS     0x3BA84Au
#define C_SUCCESS_DARK 0x2C7F38u  /* success button resting state */

/* Surfaces */
#define C_PANEL       0x102445u
#define C_PANEL_EDGE  0x2E507Cu
#define C_IDLE_MASK   0x06182Fu
#define C_DAY_ON      0x1A5CB8u   /* schedule day: active */
#define C_DAY_OFF     0x182035u   /* schedule day: inactive */

#define SCREEN_W 800
#define SCREEN_H 480
