#pragma once

/**
 * Touch driver configuration for ELECROW DIS08070H
 * Touch IC: GT911 capacitive (I2C SDA=19, SCL=20)
 *
 * Uses TAMC_GT911 library: https://github.com/TAMCTec/gt911-arduino
 */

#include <Wire.h>
#include <TAMC_GT911.h>
#include "lgfx.h"    /* for lcd.width() / lcd.height() in touch_touched() */

/* GT911 I2C wiring on DIS08070H */
#define TOUCH_GT911_SDA  19
#define TOUCH_GT911_SCL  20
#define TOUCH_GT911_INT  -1   /* Not connected */
#define TOUCH_GT911_RST  -1   /* Not connected */

/* Screen dimensions for touch coordinate mapping */
#define TOUCH_MAP_X_MAX  800
#define TOUCH_MAP_Y_MAX  480

/* Defined in lgfx.cpp — extern here for touch callbacks */
extern int touch_last_x;
extern int touch_last_y;
extern TAMC_GT911 ts;

inline void touch_init() {
    Wire.begin(TOUCH_GT911_SDA, TOUCH_GT911_SCL);
    ts.begin();
    ts.setRotation(ROTATION_NORMAL);
}

inline bool touch_has_signal() {
    return true;   /* GT911 must be polled every cycle */
}

inline bool touch_touched() {
    ts.read();
    if (ts.isTouched) {
        touch_last_x = (lcd.width() - 1) -
                       map(ts.points[0].x, 0, TOUCH_MAP_X_MAX, 0, lcd.width() - 1);
        touch_last_y = map(ts.points[0].y, 0, TOUCH_MAP_Y_MAX, lcd.height() - 1, 0);
        return true;
    }
    return false;
}

inline bool touch_released() {
    return true;   /* GT911 doesn't provide a separate release event */
}
