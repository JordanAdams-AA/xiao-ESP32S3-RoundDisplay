#pragma once
#include <lvgl.h>

/* Build the whole watch face on the active screen. Call once after lv_init()
 * and the display driver are ready.
 *
 * Two pages, swiped horizontally:
 *   0  watch   -- analog face, digital time, weekday, date
 *   1  climate -- temperature and humidity gauges
 *
 * LVGL is not thread-safe: every ui_*() call must come from the same task. */
void ui_create(void);

#define UI_PAGE_WATCH   0
#define UI_PAGE_CLIMATE 1
#define UI_PAGE_COUNT   2

/* Page control. Safe to call with any index; it is wrapped into range. */
void ui_page_set(int page, bool animate);
void ui_page_next(void);
void ui_page_prev(void);
int  ui_page_get(void);

/* Update the clock. Call once per second: the second hand ticks in discrete
 * 6-degree steps rather than sweeping. wday: 0=Sunday..6=Saturday ;
 * month: 0=Jan..11=Dec (as in struct tm).
 *
 * synced=false means NTP has not resolved yet: the hands still tick off the
 * system clock so the face is alive, but the digital readout shows "--:--"
 * instead of 1970. */
void ui_set_time(int hour, int minute, float sec, int wday, int mday, int month,
                 bool synced);

/* Update complications. valid=false shows "--" and an empty arc. */
void ui_set_temperature(float celsius, bool valid);
void ui_set_humidity(float percent, bool valid);
