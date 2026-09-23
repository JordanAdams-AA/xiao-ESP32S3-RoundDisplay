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

/* Pages sit on a grid, so navigation is directional rather than a list:
 *
 *        (0,0) watch  <-->  (1,0) climate
 *          |
 *        (0,1) display    (rotation)
 *          |
 *        (0,2) set time   (hours/minutes)
 *          |
 *        (0,3) time zone  (UTC offset)
 *
 * The settings live on their own pages rather than crowded onto one: a
 * 240px round screen cannot hold three controls and still leave touch
 * targets big enough to hit.
 */
#define UI_PAGE_WATCH    0
#define UI_PAGE_CLIMATE  1
#define UI_PAGE_SETTINGS 2
#define UI_PAGE_SETTIME  3
#define UI_PAGE_TZ       4

/* Move by one grid step. Ignored when there is no tile in that direction,
 * so the face never scrolls to an empty cell. */
void ui_nav(int dcol, int drow);
int  ui_page_get(void);

/* Rotation, 0..3 = 0/90/180/270 degrees clockwise. ui_set_rotation() updates
 * the settings page and then calls the handler, which is what actually turns
 * the panel and the touch mapping. */
typedef void (*ui_rotate_cb_t)(uint8_t rotation);
void    ui_set_rotate_handler(ui_rotate_cb_t cb);
void    ui_set_rotation(uint8_t rotation);
uint8_t ui_get_rotation(void);

/* Manual clock setting. The arrows shift the clock itself rather than
 * editing a scratch value, so the page always shows the real time and
 * there is nothing to confirm. Deltas are in minutes. */
typedef void (*ui_time_adjust_cb_t)(int delta_minutes);
void ui_set_time_adjust_handler(ui_time_adjust_cb_t cb);

/* UTC offset in whole hours. */
typedef void (*ui_tz_adjust_cb_t)(int delta_hours);
void ui_set_tz_adjust_handler(ui_tz_adjust_cb_t cb);
void ui_set_tz_offset(int hours);      /* refresh the label */

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
