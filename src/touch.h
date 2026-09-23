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

/* Consume the next gesture, or TG_NONE. One event per finger-down/up cycle.
 * Must be called regularly: it also maintains the live press state below. */
TouchGesture touch_poll(void);

/* Tell the driver how the display is rotated (0..3 = 0/90/180/270 degrees)
 * so raw panel coordinates are mapped into what the user actually sees.
 * Without this, both taps and swipe directions would stay locked to the
 * panel's physical axes after the UI is rotated. */
void touch_set_rotation(uint8_t rotation);

/* Live press state in rotated screen coordinates, for the LVGL input device.
 * Returns true while a finger is down; x/y are only written when true. */
bool touch_get_state(int *x, int *y);

/* Last raw gesture byte seen, for diagnosing an unexpected mapping. */
uint8_t touch_last_raw(void);
