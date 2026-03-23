#pragma once
#include <stdint.h>
#include <stdbool.h>

/**
 * LVGL full-screen snapshot as a 24-bit BMP in PSRAM.
 * Caller must free with screenshot_bmp_free().
 */
bool screenshot_bmp_capture(uint8_t **out_buf, uint32_t *out_size);
void screenshot_bmp_free(uint8_t *buf);
