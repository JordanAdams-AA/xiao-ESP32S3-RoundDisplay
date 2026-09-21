#include "ui.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"

/* ---- Palette (matches the reference: green accent on black) ---- */
#define COL_GREEN    lv_color_hex(0x7ED321)
#define COL_WHITE    lv_color_white()
#define COL_GRAY     lv_color_hex(0x9A9A9A)
#define COL_DARKGRAY lv_color_hex(0x333333)
#define COL_BLACK    lv_color_black()

static const int CX = SCREEN_W / 2;
static const int CY = SCREEN_H / 2;

/* Objects updated at runtime */
static lv_obj_t *hand_hour, *hand_min, *hand_sec;
static lv_point_t pts_hour[2], pts_min[2], pts_sec[2];
static lv_obj_t *lbl_time, *lbl_wday;
static lv_obj_t *arc_temp, *lbl_temp;
static lv_obj_t *arc_hum,  *lbl_hum;
static lv_obj_t *lbl_day,  *lbl_month;

/* Static background (tick ring + numbers), drawn once onto a canvas in PSRAM */
static lv_color_t *cbuf = NULL;

/* Last values written to the text labels, so a per-second tick does not
 * redraw text that has not changed. ui_set_time_unknown() resets these:
 * it writes the same labels, so the cache would otherwise suppress the
 * first real update and leave "--:--" on screen until the minute rolled. */
static int last_hour = -1, last_min = -1, last_wday = -1,
           last_mday = -1, last_month = -1;

/* True while the digital readout shows placeholders instead of a real time. */
static bool showing_unknown = false;

static const char *WDAYS[7]  = {"Sunday","Monday","Tuesday","Wednesday",
                                "Thursday","Friday","Saturday"};
static const char *MONTHS[12] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                 "JUL","AUG","SEP","OCT","NOV","DEC"};

/* --------------------------------------------------------------------- */
static void draw_background(lv_obj_t *parent)
{
    size_t sz = (size_t)SCREEN_W * SCREEN_H * sizeof(lv_color_t);
    cbuf = (lv_color_t *)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (!cbuf) cbuf = (lv_color_t *)malloc(sz);   /* fallback if no PSRAM */

    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, cbuf, SCREEN_W, SCREEN_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(canvas);
    lv_canvas_fill_bg(canvas, COL_BLACK, LV_OPA_COVER);

    /* 60 minute ticks; longer + brighter every 5 */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    for (int i = 0; i < 60; i++) {
        float a = i * 6.0f * (float)M_PI / 180.0f;   /* 6 deg per minute */
        bool major = (i % 5 == 0);
        int rOut = 118;
        int rIn  = major ? 105 : 112;
        ld.color = major ? COL_GRAY : COL_DARKGRAY;
        ld.width = major ? 3 : 2;
        lv_point_t p[2];
        p[0].x = CX + (int)(rIn  * sinf(a));
        p[0].y = CY - (int)(rIn  * cosf(a));
        p[1].x = CX + (int)(rOut * sinf(a));
        p[1].y = CY - (int)(rOut * cosf(a));
        lv_canvas_draw_line(canvas, p, 2, &ld);
    }

    /* Numbers 00,05,...,55 every 5 minutes (upright, centred on each mark) */
    lv_draw_label_dsc_t tl;
    lv_draw_label_dsc_init(&tl);
    tl.color = COL_GRAY;
    tl.font  = &lv_font_montserrat_14;
    tl.align = LV_TEXT_ALIGN_CENTER;
    for (int k = 0; k < 12; k++) {
        float a = k * 30.0f * (float)M_PI / 180.0f;
        int r = 92;
        int x = CX + (int)(r * sinf(a));
        int y = CY - (int)(r * cosf(a));
        char s[4];
        snprintf(s, sizeof(s), "%02d", k * 5);
        lv_canvas_draw_text(canvas, x - 14, y - 9, 28, &tl, s);
    }
}

/* --------------------------------------------------------------------- */
static lv_obj_t *make_hand(lv_obj_t *parent, lv_color_t c, int w, lv_point_t *pts)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_size(l, SCREEN_W, SCREEN_H);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_width(l, w, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    /* Give the hand real length at creation. A zero-length line renders as
     * nothing, which used to hide all three hands until the first NTP sync. */
    pts[0].x = CX; pts[0].y = CY;
    pts[1].x = CX; pts[1].y = CY - 10;
    lv_line_set_points(l, pts, 2);
    return l;
}

static void set_hand(lv_point_t *pts, lv_obj_t *obj, float angDeg, int len, int tail)
{
    float r = angDeg * (float)M_PI / 180.0f;
    float s = sinf(r), c = cosf(r);
    pts[0].x = CX - (int)(tail * s);   /* tail behind the centre */
    pts[0].y = CY + (int)(tail * c);
    pts[1].x = CX + (int)(len  * s);   /* tip */
    pts[1].y = CY - (int)(len  * c);
    lv_line_set_points(obj, pts, 2);
}

/* --------------------------------------------------------------------- */
static lv_obj_t *make_gauge(lv_obj_t *parent, int cx, int cy, lv_color_t ind,
                            int range_min, int range_max)
{
    const int d = 70;
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - d / 2, cy - d / 2);
    lv_arc_set_rotation(a, 135);          /* opening faces down (270 deg sweep) */
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, range_min, range_max);
    lv_arc_set_value(a, range_min);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);  /* display only, not interactive */

    lv_obj_set_style_arc_color(a, COL_DARKGRAY, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);

    lv_obj_set_style_arc_color(a, ind, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(a, COL_WHITE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 3, LV_PART_KNOB);
    return a;
}

static lv_obj_t *make_value_label(lv_obj_t *arc)
{
    lv_obj_t *l = lv_label_create(arc);
    lv_obj_set_style_text_color(l, COL_WHITE, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_label_set_text(l, "--");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -2);
    return l;
}

static void add_caption(lv_obj_t *arc, const char *txt)
{
    lv_obj_t *l = lv_label_create(arc);
    lv_obj_set_style_text_color(l, COL_GRAY, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 15);
}

/* --------------------------------------------------------------------- */
static void make_date(lv_obj_t *parent, int cx, int cy)
{
    const int d = 52;
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_set_size(ring, d, d);
    lv_obj_set_pos(ring, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, COL_GRAY, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lbl_day = lv_label_create(ring);
    lv_obj_set_style_text_color(lbl_day, COL_WHITE, 0);
    lv_obj_set_style_text_font(lbl_day, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl_day, "--");
    lv_obj_align(lbl_day, LV_ALIGN_CENTER, 0, -6);

    lbl_month = lv_label_create(ring);
    lv_obj_set_style_text_color(lbl_month, COL_GRAY, 0);
    lv_obj_set_style_text_font(lbl_month, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl_month, "---");
    lv_obj_align(lbl_month, LV_ALIGN_CENTER, 0, 12);
}

/* --------------------------------------------------------------------- */
void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BLACK, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    draw_background(scr);

    /* Digital time + weekday, upper half */
    lbl_time = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_time, COL_GREEN, 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 50);

    lbl_wday = lv_label_create(scr);
    lv_obj_set_style_text_color(lbl_wday, COL_GRAY, 0);
    lv_obj_set_style_text_font(lbl_wday, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_wday, "");
    lv_obj_align_to(lbl_wday, lbl_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    /* Complications, lower half (below hands) */
    arc_temp = make_gauge(scr, 76, 128, COL_GREEN, TEMP_MIN, TEMP_MAX);
    lbl_temp = make_value_label(arc_temp);
    add_caption(arc_temp, "TEMP");

    arc_hum  = make_gauge(scr, 164, 128, COL_GREEN, HUM_MIN, HUM_MAX);
    lbl_hum  = make_value_label(arc_hum);
    add_caption(arc_hum, "HUM");

    make_date(scr, 120, 182);

    /* Hands on top, then the centre hub */
    hand_hour = make_hand(scr, COL_WHITE, 6, pts_hour);
    hand_min  = make_hand(scr, COL_WHITE, 4, pts_min);
    hand_sec  = make_hand(scr, COL_GREEN, 2, pts_sec);

    lv_obj_t *hub = lv_obj_create(scr);
    lv_obj_set_size(hub, 14, 14);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, COL_GREEN, 0);
    lv_obj_set_style_border_color(hub, COL_WHITE, 0);
    lv_obj_set_style_border_width(hub, 2, 0);
    lv_obj_set_style_pad_all(hub, 0, 0);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(hub);
}

/* --------------------------------------------------------------------- */
void ui_set_time(int hour, int minute, float sec, int wday, int mday, int month,
                 bool synced)
{
    float sa = sec / 60.0f * 360.0f;
    float ma = (minute + sec / 60.0f) / 60.0f * 360.0f;
    float ha = ((hour % 12) + minute / 60.0f) / 12.0f * 360.0f;

    set_hand(pts_hour, hand_hour, ha, 55, 0);
    set_hand(pts_min,  hand_min,  ma, 90, 0);
    set_hand(pts_sec,  hand_sec,  sa, 100, 20);

    /* Before NTP resolves, the hands still tick off the system clock so the
     * face is visibly alive, but the digital readout must not show 1970.
     * Written once on entry to that state, not every second. */
    if (!synced) {
        if (!showing_unknown) {
            showing_unknown = true;
            lv_label_set_text(lbl_time,  "--:--");
            lv_label_set_text(lbl_wday,  "syncing");
            lv_label_set_text(lbl_day,   "--");
            lv_label_set_text(lbl_month, "---");
            last_hour = last_min = last_wday = last_mday = last_month = -1;
        }
        return;
    }

    /* Leaving the unknown state: the labels hold placeholders, so the cache
     * below must not suppress the first real write. */
    if (showing_unknown) {
        showing_unknown = false;
        last_hour = last_min = last_wday = last_mday = last_month = -1;
    }

    /* The hands move every second, the text almost never does. Rewriting a
     * label invalidates its area and costs another SPI flush, so only touch
     * the ones whose value actually changed. */
    if (hour != last_hour || minute != last_min) {
        last_hour = hour; last_min = minute;
        char t[8];
        snprintf(t, sizeof(t), "%02d:%02d", hour, minute);
        lv_label_set_text(lbl_time, t);
    }

    if (wday != last_wday && wday >= 0 && wday < 7) {
        last_wday = wday;
        lv_label_set_text(lbl_wday, WDAYS[wday]);
    }

    if (mday != last_mday) {
        last_mday = mday;
        char d[4];
        snprintf(d, sizeof(d), "%d", mday);
        lv_label_set_text(lbl_day, d);
    }

    if (month != last_month && month >= 0 && month < 12) {
        last_month = month;
        lv_label_set_text(lbl_month, MONTHS[month]);
    }
}

/* --------------------------------------------------------------------- */
static void set_gauge(lv_obj_t *arc, lv_obj_t *lbl, float v, bool valid,
                      int lo, int hi)
{
    if (!valid) {
        lv_arc_set_value(arc, lo);
        lv_label_set_text(lbl, "--");
        return;
    }
    int vi = (int)lroundf(v);
    int cl = vi;
    if (cl < lo) cl = lo;
    if (cl > hi) cl = hi;
    lv_arc_set_value(arc, cl);
    char s[8];
    snprintf(s, sizeof(s), "%d", vi);   /* real value as text, even if clamped */
    lv_label_set_text(lbl, s);
}

void ui_set_temperature(float celsius, bool valid)
{
    set_gauge(arc_temp, lbl_temp, celsius, valid, TEMP_MIN, TEMP_MAX);
}

void ui_set_humidity(float percent, bool valid)
{
    set_gauge(arc_hum, lbl_hum, percent, valid, HUM_MIN, HUM_MAX);
}
