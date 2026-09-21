#pragma once
#include <lvgl.h>

/* Build the whole watch face on the active screen. Call once after lv_init()
 * and the display driver are ready. */
void ui_create(void);

/* Update the clock. Call once per second: the second hand ticks in discrete
 * 6-degree steps rather than sweeping. wday: 0=Sunday..6=Saturday ;
 * month: 0=Jan..11=Dec (as in struct tm).
 *
 * synced=false means NTP has not resolved yet: the hands still tick off the
 * system clock so the face is alive, but the digital readout shows "--:--"
 * instead of 1970. All UI calls must come from a single task -- LVGL is not
 * thread-safe. */
void ui_set_time(int hour, int minute, float sec, int wday, int mday, int month,
                 bool synced);

/* Update complications. valid=false shows "--" and an empty arc. */
void ui_set_temperature(float celsius, bool valid);
void ui_set_humidity(float percent, bool valid);
