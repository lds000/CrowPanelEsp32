#pragma once

/**
 * LGFX display driver header for ELECROW DIS08070H
 * ESP32-S3 RGB parallel interface + LVGL draw buffer setup
 * Display: 800x480, drivers EK9716BD3 + EK73002ACGB
 */

/* Arduino.h must come first — it defines ESP_PLATFORM which LovyanGFX uses
   to detect the target SoC and include the correct ESP32-S3 platform headers */
#include <Arduino.h>
#include <LovyanGFX.hpp>
/* Explicitly include Bus_RGB and Panel_RGB — LovyanGFX 1.1.x does not
   auto-include these via device.hpp; they must be pulled in manually */
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lvgl.h>

#define TFT_BL  2   /* Backlight pin */

class LGFX : public lgfx::LGFX_Device {
private:
    lgfx::Bus_RGB   _bus_instance;
    lgfx::Panel_RGB _panel_instance;

    uint32_t _screen_width;
    uint32_t _screen_height;
    uint32_t _disp_buf_pixels;

    lv_disp_draw_buf_t _draw_buf;
    /* Allocated at runtime so it can live in PSRAM instead of scarce DRAM */
    lv_color_t *_disp_buf;
    lv_disp_drv_t  _disp_drv;

public:
    LGFX(void);
    void setup();
};

extern LGFX lcd;
