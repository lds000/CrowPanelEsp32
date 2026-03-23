/**
 * Shared: LVGL snapshot -> 24-bit BMP (PSRAM).
 */

#include "screenshot_bmp.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "lgfx.h"

static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void rgb_bus_word_to_bgr(uint16_t px, uint8_t *bgr) {
    /* Bus_RGB stores scanout words in the panel's physical lane order, not
       standard RGB565 memory order. Reconstruct logical RGB from the wired bus:
       R[4:0] <- bits 7..3, G[5:0] <- bits 2..0 + 15..13, B[4:0] <- bits 12..8. */
    uint8_t r5 = (px >> 3) & 0x1F;
    uint8_t g6 = (uint8_t)(((px & 0x0007u) << 3) | ((px >> 13) & 0x07u));
    uint8_t b5 = (px >> 8) & 0x1F;
    bgr[2] = (uint8_t)((r5 << 3) | (r5 >> 2));
    bgr[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
    bgr[0] = (uint8_t)((b5 << 3) | (b5 >> 2));
}

bool screenshot_bmp_capture(uint8_t **out_buf, uint32_t *out_size) {
    *out_buf  = nullptr;
    *out_size = 0;

    int w = lcd.width();
    int h = lcd.height();
    if (w <= 0 || h <= 0) {
        return false;
    }

    uint32_t rowSize  = ((w * 3 + 3) & ~3u);
    uint32_t imageSize = rowSize * (uint32_t)h;
    uint32_t fileSize  = 54 + imageSize;

    uint8_t *bmp = (uint8_t *)heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!bmp) {
        return false;
    }

    const uint16_t *fb = lcd.frameBuffer565();
    if (!fb) {
        heap_caps_free(bmp);
        return false;
    }

    memset(bmp, 0, fileSize);

    bmp[0] = 'B';
    bmp[1] = 'M';
    write_le32(bmp + 2, fileSize);
    bmp[6] = bmp[7] = bmp[8] = bmp[9] = 0;
    write_le32(bmp + 10, 54);

    write_le32(bmp + 14, 40);
    write_le32(bmp + 18, (uint32_t)w);
    int32_t neg_h = -(int32_t)h;
    memcpy(bmp + 22, &neg_h, 4);
    write_le16(bmp + 26, 1);
    write_le16(bmp + 28, 24);
    write_le32(bmp + 30, 0);
    write_le32(bmp + 34, imageSize);
    write_le32(bmp + 38, 0);
    write_le32(bmp + 42, 0);
    write_le32(bmp + 46, 0);
    write_le32(bmp + 50, 0);

    uint8_t *dst = bmp + 54;

    for (int y = 0; y < h; y++) {
        uint8_t *row = dst + y * rowSize;
        const uint16_t *src = fb + ((size_t)y * (size_t)w);
        for (int x = 0; x < w; ++x) {
            rgb_bus_word_to_bgr(src[x], row + x * 3);
        }
        if ((y & 31) == 31) delay(1);
    }

    *out_buf  = bmp;
    *out_size = fileSize;
    return true;
}

void screenshot_bmp_free(uint8_t *buf) {
    if (buf) heap_caps_free(buf);
}
