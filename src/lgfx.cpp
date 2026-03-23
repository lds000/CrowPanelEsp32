/**
 * LGFX display driver implementation for ELECROW DIS08070H
 *
 * RGB parallel bus pin mapping (ESP32-S3):
 *   B[4:0] → GPIO 4,5,6,7,15
 *   G[5:0] → GPIO 9,46,3,8,16,1
 *   R[4:0] → GPIO 14,21,47,48,45
 *   HSYNC  → GPIO 39 | VSYNC → GPIO 40 | DE → GPIO 41 | PCLK → GPIO 0
 *
 * Backlight: GPIO 2 (PWM via LEDC channel 1)
 */

#include "lgfx.h"
#include "touch.h"
#include "app_state.h"
#include <esp_heap_caps.h>

LGFX lcd;
volatile uint32_t g_last_touch_ms = 0;

/* Touch driver globals — defined here, declared extern in touch.h */
TAMC_GT911 ts(TOUCH_GT911_SDA, TOUCH_GT911_SCL,
              TOUCH_GT911_INT, TOUCH_GT911_RST,
              TOUCH_MAP_X_MAX, TOUCH_MAP_Y_MAX);
int touch_last_x = 0;
int touch_last_y = 0;

/* ── LVGL display flush callback ──────────────────────────── */
static void disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    lcd.pushImageDMA(area->x1, area->y1, w, h, reinterpret_cast<lgfx::rgb565_t *>(&color_p->full));
    lv_disp_flush_ready(drv);
}

/* ── LVGL touch read callback ─────────────────────────────── */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    if (touch_has_signal()) {
        if (touch_touched()) {
            g_last_touch_ms = millis();
            data->state   = LV_INDEV_STATE_PR;
            data->point.x = touch_last_x;
            data->point.y = touch_last_y;
        } else if (touch_released()) {
            data->state = LV_INDEV_STATE_REL;
        }
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    delay(5);
}

/* ── LGFX constructor: configure the RGB bus + panel ──────── */
LGFX::LGFX(void) {
    _disp_buf = nullptr;
    _disp_buf_pixels = 0;

    /* RGB bus config */
    {
        auto cfg = _bus_instance.config();
        cfg.panel = &_panel_instance;

        /* Blue channel (B0–B4) */
        cfg.pin_d0  = GPIO_NUM_15;
        cfg.pin_d1  = GPIO_NUM_7;
        cfg.pin_d2  = GPIO_NUM_6;
        cfg.pin_d3  = GPIO_NUM_5;
        cfg.pin_d4  = GPIO_NUM_4;

        /* Green channel (G0–G5) */
        cfg.pin_d5  = GPIO_NUM_9;
        cfg.pin_d6  = GPIO_NUM_46;
        cfg.pin_d7  = GPIO_NUM_3;
        cfg.pin_d8  = GPIO_NUM_8;
        cfg.pin_d9  = GPIO_NUM_16;
        cfg.pin_d10 = GPIO_NUM_1;

        /* Red channel (R0–R4) */
        cfg.pin_d11 = GPIO_NUM_14;
        cfg.pin_d12 = GPIO_NUM_21;
        cfg.pin_d13 = GPIO_NUM_47;
        cfg.pin_d14 = GPIO_NUM_48;
        cfg.pin_d15 = GPIO_NUM_45;

        cfg.pin_henable = GPIO_NUM_41;
        cfg.pin_vsync   = GPIO_NUM_40;
        cfg.pin_hsync   = GPIO_NUM_39;
        cfg.pin_pclk    = GPIO_NUM_0;
        cfg.freq_write  = 15000000;   /* 15 MHz pixel clock */

        /* Timing for EK9716BD3 800x480 panel */
        cfg.hsync_polarity    = 0;
        cfg.hsync_front_porch = 40;
        cfg.hsync_pulse_width = 48;
        cfg.hsync_back_porch  = 40;

        cfg.vsync_polarity    = 0;
        cfg.vsync_front_porch = 1;
        cfg.vsync_pulse_width = 31;
        cfg.vsync_back_porch  = 13;

        cfg.pclk_active_neg = 1;
        cfg.de_idle_high    = 0;
        cfg.pclk_idle_high  = 0;

        _bus_instance.config(cfg);
    }

    /* Panel config */
    {
        auto cfg = _panel_instance.config();
        cfg.memory_width  = 800;
        cfg.memory_height = 480;
        cfg.panel_width   = 800;
        cfg.panel_height  = 480;
        cfg.offset_x      = 0;
        cfg.offset_y      = 0;
        _panel_instance.config(cfg);
    }

    _panel_instance.setBus(&_bus_instance);
    setPanel(&_panel_instance);
}

/* ── LGFX::setup() — init display + LVGL + touch ─────────── */
void LGFX::setup() {
    /* Initialise the physical display */
    this->begin();
    this->fillScreen(TFT_BLACK);
    _screen_width  = this->width();   /* 800 */
    _screen_height = this->height();  /* 480 */

    /* Backlight: fade up using LEDC PWM */
    ledcSetup(1, 300, 8);
    ledcAttachPin(TFT_BL, 1);
    for (int v = 0; v <= 255; v += 5) {
        ledcWrite(1, v);
        delay(4);
    }

    /* LVGL init */
    lv_init();

    /* Allocate the draw buffer in PSRAM first, with an internal-RAM fallback. */
    _disp_buf_pixels = _screen_width * _screen_height / 20;   /* ~38 KB RGB565 */
    _disp_buf = static_cast<lv_color_t *>(
        heap_caps_malloc(_disp_buf_pixels * sizeof(lv_color_t),
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_disp_buf == nullptr) {
        _disp_buf = static_cast<lv_color_t *>(
            heap_caps_malloc(_disp_buf_pixels * sizeof(lv_color_t),
                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    if (_disp_buf == nullptr) {
        Serial.println("[LCD] Failed to allocate LVGL draw buffer");
        while (true) delay(1000);
    }

    /* Register display driver */
    lv_disp_draw_buf_init(&_draw_buf, _disp_buf, nullptr, _disp_buf_pixels);
    lv_disp_drv_init(&_disp_drv);
    _disp_drv.hor_res  = _screen_width;
    _disp_drv.ver_res  = _screen_height;
    _disp_drv.flush_cb = disp_flush_cb;
    _disp_drv.draw_buf = &_draw_buf;
    lv_disp_drv_register(&_disp_drv);

    /* Register touch input driver */
    touch_init();
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);
}

const uint16_t *LGFX::frameBuffer565() {
    return reinterpret_cast<const uint16_t *>(_bus_instance.getDMABuffer(0));
}
