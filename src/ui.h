#pragma once
#include <lvgl.h>

/* Build the whole watch face on the active screen. Call once after lv_init()
 * and the display driver are ready. */
void ui_create(void);

/* Update the clock. sec is a float so the second hand can sweep smoothly.
 * wday: 0=Sunday..6=Saturday ; month: 0=Jan..11=Dec (as in struct tm). */
void ui_set_time(int hour, int minute, float sec, int wday, int mday, int month);

/* Update complications. valid=false shows "--" and an empty arc. */
void ui_set_temperature(float celsius, bool valid);
void ui_set_humidity(float percent, bool valid);

/* Shown before the first NTP sync. */
void ui_set_time_unknown(void);
