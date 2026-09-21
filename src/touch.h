#pragma once
#include <stdint.h>

/* Capacitive touch on the Seeed Round Display (CST816-family, I2C).
 *
 * The controller reports gestures itself, so swipes are read straight out of
 * its gesture register rather than being reconstructed from coordinates. That
 * avoids depending on the panel's axis orientation matching the display's. */
enum TouchGesture {
    TG_NONE = 0,
    TG_UP,
    TG_DOWN,
    TG_LEFT,
    TG_RIGHT,
    TG_TAP,
    TG_LONG,
};

/* Probe the bus and latch whether a controller answered. Safe to call once
 * from setup(); returns the same value touch_present() will report. */
bool touch_init(void);

/* False when no controller answered at TOUCH_I2C_ADDR. */
bool touch_present(void);

/* Consume the next gesture, or TG_NONE. One event per finger-down/up cycle:
 * the controller holds its gesture register, so it is only reported once. */
TouchGesture touch_poll(void);

/* Last raw gesture byte seen, for diagnosing an unexpected mapping. */
uint8_t touch_last_raw(void);
