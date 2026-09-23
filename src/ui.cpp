#include "ui.h"
#include "config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include <Arduino.h>

/* ---- Palette (matches the reference: green accent on black) ---- */
#define COL_GREEN    lv_color_hex(0x7ED321)
#define COL_WHITE    lv_color_white()
#define COL_GRAY     lv_color_hex(0x9A9A9A)
#define COL_DARKGRAY lv_color_hex(0x333333)
#define COL_BLACK    lv_color_black()
#define COL_BLUE     lv_color_hex(0x2E9BE6)   /* humidity indicator */

/* Temperature ramp. 0 / 20 / 30 are the anchors that were asked for; the
 * green stop at 10 exists because lerping light blue straight to orange
 * passes through a muddy khaki around the midpoint. */
#define COL_T_COLD   lv_color_hex(0x7EC8E3)   /*  <= 0 C  light blue */
#define COL_T_MILD   lv_color_hex(0x7ED321)   /*    10 C  green      */
#define COL_T_WARM   lv_color_hex(0xF5A623)   /*    20 C  yellow-orange */
#define COL_T_HOT    lv_color_hex(0xE0322A)   /*  >= 30 C red        */

/* Blend a..b by t in [0,1]. lv_color_mix weights its FIRST argument by the
 * mix value, so the endpoints go in reversed. */
static lv_color_t lerp(lv_color_t a, lv_color_t b, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return lv_color_mix(b, a, (uint8_t)lroundf(t * 255.0f));
}

static lv_color_t temp_color(float c)
{
    if (c <= 0.0f)  return COL_T_COLD;
    if (c >= 30.0f) return COL_T_HOT;
    if (c < 10.0f)  return lerp(COL_T_COLD, COL_T_MILD, c / 10.0f);
    if (c < 20.0f)  return lerp(COL_T_MILD, COL_T_WARM, (c - 10.0f) / 10.0f);
    return lerp(COL_T_WARM, COL_T_HOT, (c - 20.0f) / 10.0f);
}

static const int CX = SCREEN_W / 2;
static const int CY = SCREEN_H / 2;

/* ---- Pages ---- */
static lv_obj_t *tileview;
static lv_obj_t *tile_watch, *tile_climate, *tile_settings;
static int cur_col = 0, cur_row = 0;

/* ---- Settings ---- */
static lv_obj_t *lbl_rot_value;
static uint8_t   rotation = 0;
static ui_rotate_cb_t rotate_cb = NULL;

/* ---- Page 0: watch ---- */
static lv_obj_t *hand_hour, *hand_min, *hand_sec;
static lv_point_t pts_hour[2], pts_min[2], pts_sec[2];
static lv_obj_t *lbl_time, *lbl_wday;
static lv_obj_t *lbl_day,  *lbl_month;

/* Face read-outs: icon + whole number + a smaller decimal, so the value
 * stays legible without eating the space either side of the hub. */
#define ICON_W 15
#define ICON_H 19
static lv_obj_t *icon_fire, *icon_drop;
static lv_obj_t *lbl_ft_int, *lbl_ft_dec;   /* face temperature */
static lv_obj_t *lbl_fh_int, *lbl_fh_dec;   /* face humidity    */
/* TRUE_COLOR, not TRUE_COLOR_ALPHA: LVGL only supports drawing onto an
 * alpha canvas at 32-bit colour depth, and this build is 16-bit. The page
 * behind is black anyway, so an opaque black backdrop is indistinguishable
 * from transparency here. */
static uint8_t   icon_fire_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(ICON_W, ICON_H)];
static uint8_t   icon_drop_buf[LV_CANVAS_BUF_SIZE_TRUE_COLOR(ICON_W, ICON_H)];
static lv_color_t fire_col_now;
static bool       fire_col_valid = false;

/* ---- Page 1: climate ---- */
static lv_obj_t *arc_temp, *lbl_temp;
static lv_obj_t *arc_hum,  *lbl_hum;
static lv_obj_t *lbl_clim_time;

/* Static background (tick ring + numbers), drawn once onto a canvas in PSRAM */
static lv_color_t *cbuf = NULL;

/* Last values written to the text labels, so a per-second tick does not
 * redraw text that has not changed. Reset when leaving the pre-NTP state:
 * the placeholder labels would otherwise suppress the first real update. */
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

/* ---------------------------------------------------------------------
 * Icons. LVGL ships a droplet (LV_SYMBOL_TINT) but no flame, so both are
 * drawn as polygons instead -- mixing a font glyph with a drawn shape
 * would read as two different styles sitting side by side.
 * Coordinates are in a 15x19 box; the flame has three tongues so it is not
 * mistaken for the droplet at this size. */
static const lv_point_t FLAME_PTS[] = {
    {7, 0}, {9, 5}, {11, 2}, {12, 8}, {14, 12},
    {11, 18}, {4, 18}, {1, 12}, {3, 7}, {4, 2}, {6, 5},
};
static const lv_point_t DROP_PTS[] = {
    {7, 0}, {9, 5}, {12, 10}, {12, 14}, {9, 18},
    {5, 18}, {2, 14}, {2, 10}, {5, 5},
};

/* Scanline fill straight into the canvas buffer.
 *
 * lv_canvas_draw_polygon() hangs on these outlines: LVGL's software polygon
 * renderer is built for convex shapes, and the flame's tongues are concave.
 * An even-odd scanline fill handles concavity correctly, and writing the
 * pixels directly is far cheaper than LVGL's mask machinery for a 15x19 icon.
 */
static void draw_icon(uint8_t *buf, lv_obj_t *canvas,
                      const lv_point_t *p, int n, lv_color_t col)
{
    lv_color_t *px = (lv_color_t *)buf;
    memset(buf, 0, (size_t)ICON_W * ICON_H * sizeof(lv_color_t));  /* black */

    for (int y = 0; y < ICON_H; y++) {
        float xs[16];
        int   cnt = 0;
        float fy  = (float)y + 0.5f;        /* sample at pixel centres */

        for (int i = 0; i < n && cnt < 16; i++) {
            const lv_point_t *a = &p[i];
            const lv_point_t *b = &p[(i + 1) % n];
            if (a->y == b->y) continue;     /* horizontal edges add nothing */
            /* Half-open test so a vertex shared by two edges counts once. */
            if ((fy >= a->y && fy < b->y) || (fy >= b->y && fy < a->y)) {
                float t = (fy - (float)a->y) / (float)(b->y - a->y);
                xs[cnt++] = (float)a->x + t * (float)(b->x - a->x);
            }
        }

        for (int i = 1; i < cnt; i++) {     /* insertion sort, cnt is tiny */
            float k = xs[i];
            int   j = i - 1;
            while (j >= 0 && xs[j] > k) { xs[j + 1] = xs[j]; j--; }
            xs[j + 1] = k;
        }

        for (int i = 0; i + 1 < cnt; i += 2) {
            int x0 = (int)ceilf(xs[i] - 0.5f);
            int x1 = (int)floorf(xs[i + 1] - 0.5f);
            if (x0 < 0) x0 = 0;
            if (x1 > ICON_W - 1) x1 = ICON_W - 1;
            for (int x = x0; x <= x1; x++) px[y * ICON_W + x] = col;
        }
    }
    lv_obj_invalidate(canvas);
}

static lv_obj_t *make_icon(lv_obj_t *parent, uint8_t *buf,
                           const lv_point_t *pts, int n, lv_color_t col)
{
    lv_obj_t *c = lv_canvas_create(parent);
    lv_canvas_set_buffer(c, buf, ICON_W, ICON_H, LV_IMG_CF_TRUE_COLOR);
    draw_icon(buf, c, pts, n, col);
    return c;
}

/* icon + "21" + ".5" on one baseline. Flex keeps the group centred on its
 * anchor however wide the number happens to be. */
static lv_obj_t *make_readout(lv_obj_t *parent, int dx, uint8_t *buf,
                              const lv_point_t *pts, int npts,
                              lv_color_t icon_col,
                              lv_obj_t **icon_out,
                              lv_obj_t **int_out, lv_obj_t **dec_out)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    /* Cross-axis END bottom-aligns them, so the small decimal sits on the
     * same baseline as the big number instead of floating mid-height. */
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 2, 0);
    lv_obj_align(box, LV_ALIGN_CENTER, dx, 0);

    *icon_out = make_icon(box, buf, pts, npts, icon_col);

    lv_obj_t *i = lv_label_create(box);
    lv_obj_set_style_text_color(i, COL_WHITE, 0);
    lv_obj_set_style_text_font(i, &lv_font_montserrat_16, 0);
    lv_label_set_text(i, "--");
    *int_out = i;

    lv_obj_t *d = lv_label_create(box);
    lv_obj_set_style_text_color(d, COL_GRAY, 0);
    lv_obj_set_style_text_font(d, &lv_font_montserrat_12, 0);
    lv_label_set_text(d, "");
    *dec_out = d;

    return box;
}

/* Split for display: "-2.5" becomes "-2" and ".5". Sign is handled before
 * the split so the integer part does not round the wrong way negative. */
static void split_value(float v, char *ip, size_t ipn, char *dp, size_t dpn)
{
    bool neg = v < 0.0f;
    float a  = fabsf(v);
    int   w  = (int)a;
    int   f  = (int)lroundf((a - (float)w) * 10.0f);
    if (f >= 10) { w += 1; f = 0; }
    snprintf(ip, ipn, "%s%d", neg && (w || f) ? "-" : "", w);
    snprintf(dp, dpn, ".%d", f);
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
static lv_obj_t *make_gauge(lv_obj_t *parent, int cx, int cy, int d,
                            lv_color_t ind, int range_min, int range_max)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, d, d);
    lv_obj_set_pos(a, cx - d / 2, cy - d / 2);
    lv_arc_set_rotation(a, 135);          /* opening faces down (270 deg sweep) */
    lv_arc_set_bg_angles(a, 0, 270);
    /* Arc values are integers, so the range is held in tenths: the needle
     * then tracks the same resolution the label prints. */
    lv_arc_set_range(a, range_min * 10, range_max * 10);
    lv_arc_set_value(a, range_min * 10);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);  /* display only */

    lv_obj_set_style_arc_color(a, COL_DARKGRAY, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);

    lv_obj_set_style_arc_color(a, ind, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(a, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);

    /* No knob: this is a read-out, and the dot reads as a drag handle. */
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    return a;
}

/* --------------------------------------------------------------------- */
static void build_watch_page(lv_obj_t *parent)
{
    draw_background(parent);

    /* Digital time high on the face, clear of the "00" marker above it. */
    lbl_time = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_time, COL_GREEN, 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_28, 0);
    lv_label_set_text(lbl_time, "--:--");
    lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, 0, 48);

    /* Fixed offset rather than align_to(): the weekday is the widest string
     * on the face and used to run under the humidity gauge that sat here. */
    lbl_wday = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_wday, COL_GRAY, 0);
    lv_obj_set_style_text_font(lbl_wday, &lv_font_montserrat_16, 0);
    lv_label_set_text(lbl_wday, "");
    lv_obj_align(lbl_wday, LV_ALIGN_TOP_MID, 0, 80);

    /* Date ring. Raised from y=182 to y=164: at the old position its lower
     * edge covered the "30" minute number on the ring below it. */
    const int d = 52;
    lv_obj_t *ring = lv_obj_create(parent);
    lv_obj_set_size(ring, d, d);
    lv_obj_set_pos(ring, CX - d / 2, 164 - d / 2);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(ring, COL_GRAY, 0);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_pad_all(ring, 0, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lbl_day = lv_label_create(ring);
    lv_obj_set_style_text_color(lbl_day, COL_WHITE, 0);
    lv_obj_set_style_text_font(lbl_day, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_day, "--");
    lv_obj_align(lbl_day, LV_ALIGN_CENTER, 0, -7);

    lbl_month = lv_label_create(ring);
    lv_obj_set_style_text_color(lbl_month, COL_GRAY, 0);
    lv_obj_set_style_text_font(lbl_month, &lv_font_montserrat_12, 0);
    lv_label_set_text(lbl_month, "---");
    lv_obj_align(lbl_month, LV_ALIGN_CENTER, 0, 11);

    /* Read-outs on the 9-3 line: temperature between the "45" marker and the
     * hub, humidity between the hub and "15". Created before the hands so the
     * hands sweep over them rather than under. */
    make_readout(parent, -46, icon_fire_buf, FLAME_PTS,
                 (int)(sizeof(FLAME_PTS) / sizeof(FLAME_PTS[0])), COL_T_COLD,
                 &icon_fire, &lbl_ft_int, &lbl_ft_dec);
    make_readout(parent,  46, icon_drop_buf, DROP_PTS,
                 (int)(sizeof(DROP_PTS) / sizeof(DROP_PTS[0])), COL_BLUE,
                 &icon_drop, &lbl_fh_int, &lbl_fh_dec);

    /* Hands on top, then the centre hub */
    hand_hour = make_hand(parent, COL_WHITE, 6, pts_hour);
    hand_min  = make_hand(parent, COL_WHITE, 4, pts_min);
    hand_sec  = make_hand(parent, COL_GREEN, 2, pts_sec);

    lv_obj_t *hub = lv_obj_create(parent);
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
static void add_climate_block(lv_obj_t *parent, int cx, const char *caption,
                              lv_color_t indicator, int range_min, int range_max,
                              lv_obj_t **arc_out, lv_obj_t **val_out)
{
    const int d  = 88;
    const int cy = 124;

    lv_obj_t *a = make_gauge(parent, cx, cy, d, indicator, range_min, range_max);
    *arc_out = a;

    /* montserrat_24, not 28: the widest reading is now "100.0", and the arc
     * leaves only ~74px of clear width inside its stroke. */
    lv_obj_t *v = lv_label_create(a);
    lv_obj_set_style_text_color(v, COL_WHITE, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_24, 0);
    lv_label_set_text(v, "--");
    lv_obj_align(v, LV_ALIGN_CENTER, 0, -8);
    *val_out = v;

    lv_obj_t *c = lv_label_create(a);
    lv_obj_set_style_text_color(c, COL_GRAY, 0);
    lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);
    lv_label_set_text(c, caption);
    lv_obj_align(c, LV_ALIGN_CENTER, 0, 18);
}

static void build_climate_page(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* Small clock kept here so the page is still glanceable as a watch. */
    lbl_clim_time = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_clim_time, COL_GREEN, 0);
    lv_obj_set_style_text_font(lbl_clim_time, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_clim_time, "--:--");
    lv_obj_align(lbl_clim_time, LV_ALIGN_TOP_MID, 0, 36);

    /* Both gauges sit on the same centre line, inset far enough that their
     * outer edges stay inside the round bezel. */
    /* Temperature starts at the cold end and is recoloured per reading. */
    add_climate_block(parent,  68, "TEMP C", COL_T_COLD,
                      TEMP_MIN, TEMP_MAX, &arc_temp, &lbl_temp);
    add_climate_block(parent, 172, "HUM %", COL_BLUE,
                      HUM_MIN, HUM_MAX, &arc_hum, &lbl_hum);
}

/* --------------------------------------------------------------------- */
static void rotate_clicked(lv_event_t *e)
{
    LV_UNUSED(e);
    ui_set_rotation((uint8_t)((rotation + 1) & 3));
}

static void build_settings_page(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_obj_set_style_text_color(title, COL_GRAY, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_label_set_text(title, "SETTINGS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 46);

    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 132, 52);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(btn, 26, 0);
    lv_obj_set_style_bg_color(btn, COL_DARKGRAY, 0);
    lv_obj_set_style_border_color(btn, COL_GREEN, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_add_event_cb(btn, rotate_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bl = lv_label_create(btn);
    lv_obj_set_style_text_color(bl, COL_WHITE, 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_label_set_text(bl, "ROTATE");
    lv_obj_center(bl);

    /* Montserrat's built-in ranges are ASCII, so no degree glyph. */
    lbl_rot_value = lv_label_create(parent);
    lv_obj_set_style_text_color(lbl_rot_value, COL_GREEN, 0);
    lv_obj_set_style_text_font(lbl_rot_value, &lv_font_montserrat_20, 0);
    lv_label_set_text(lbl_rot_value, "0 deg");
    lv_obj_align(lbl_rot_value, LV_ALIGN_CENTER, 0, 44);

    lv_obj_t *hint = lv_label_create(parent);
    lv_obj_set_style_text_color(hint, COL_GRAY, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_label_set_text(hint, "swipe down for watch");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -42);
}

/* --------------------------------------------------------------------- */
void ui_set_rotate_handler(ui_rotate_cb_t cb) { rotate_cb = cb; }
uint8_t ui_get_rotation(void) { return rotation; }

void ui_set_rotation(uint8_t r)
{
    rotation = r & 3;

    char s[12];
    snprintf(s, sizeof(s), "%d deg", rotation * 90);
    lv_label_set_text(lbl_rot_value, s);

    /* The panel keeps its pixels; only the scan mapping changes. Nothing in
     * LVGL's dirty-area tracking knows that, so repaint everything. */
    if (rotate_cb) rotate_cb(rotation);
    lv_obj_invalidate(lv_scr_act());
}

/* --------------------------------------------------------------------- */
void ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BLACK, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    tileview = lv_tileview_create(scr);
    lv_obj_set_size(tileview, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(tileview, COL_BLACK, 0);
    lv_obj_set_style_bg_opa(tileview, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(tileview, LV_SCROLLBAR_MODE_OFF);

    /* LV_DIR_NONE: paging is driven from the touch driver's own gesture
     * detection, not LVGL's drag-scroll. An LVGL input device is registered
     * so buttons work, and letting the tileview also scroll on drag would
     * fight that handler and double-trigger page changes. */
    tile_watch    = lv_tileview_add_tile(tileview, 0, 0, LV_DIR_NONE);
    tile_climate  = lv_tileview_add_tile(tileview, 1, 0, LV_DIR_NONE);
    tile_settings = lv_tileview_add_tile(tileview, 0, 1, LV_DIR_NONE);

    lv_obj_t *tiles[3] = { tile_watch, tile_climate, tile_settings };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_pad_all(tiles[i], 0, 0);
        lv_obj_set_style_border_width(tiles[i], 0, 0);
        lv_obj_set_scrollbar_mode(tiles[i], LV_SCROLLBAR_MODE_OFF);
        lv_obj_clear_flag(tiles[i], LV_OBJ_FLAG_SCROLLABLE);
    }

    build_watch_page(tile_watch);
    build_climate_page(tile_climate);
    build_settings_page(tile_settings);

    cur_col = cur_row = 0;
    lv_obj_set_tile_id(tileview, 0, 0, LV_ANIM_OFF);
}

/* --------------------------------------------------------------------- */
/* Only these grid cells exist; anything else is a no-op rather than a scroll
 * into empty space. */
static bool cell_exists(int col, int row)
{
    return (col == 0 && row == 0) ||   /* watch    */
           (col == 1 && row == 0) ||   /* climate  */
           (col == 0 && row == 1);     /* settings */
}

void ui_nav(int dcol, int drow)
{
    int c = cur_col + dcol;
    int r = cur_row + drow;
    if (!cell_exists(c, r)) return;

    cur_col = c;
    cur_row = r;
    lv_obj_set_tile_id(tileview, (uint32_t)c, (uint32_t)r, LV_ANIM_ON);
}

int ui_page_get(void)
{
    if (cur_row == 1) return UI_PAGE_SETTINGS;
    return cur_col == 1 ? UI_PAGE_CLIMATE : UI_PAGE_WATCH;
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
            lv_label_set_text(lbl_time,      "--:--");
            lv_label_set_text(lbl_clim_time, "--:--");
            lv_label_set_text(lbl_wday,      "syncing");
            lv_label_set_text(lbl_day,       "--");
            lv_label_set_text(lbl_month,     "---");
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
        lv_label_set_text(lbl_clim_time, t);
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
        lv_arc_set_value(arc, lo * 10);
        lv_label_set_text(lbl, "--");
        return;
    }
    float cl = v;
    if (cl < (float)lo) cl = (float)lo;
    if (cl > (float)hi) cl = (float)hi;
    lv_arc_set_value(arc, (int)lroundf(cl * 10.0f));

    char s[16];
    snprintf(s, sizeof(s), "%.1f", v);  /* real value as text, even if clamped */
    lv_label_set_text(lbl, s);
}

/* Face read-out: whole number big, decimal small. */
static void set_readout(lv_obj_t *ip, lv_obj_t *dp, float v, bool valid)
{
    if (!valid) {
        lv_label_set_text(ip, "--");
        lv_label_set_text(dp, "");
        return;
    }
    char a[8], b[8];
    split_value(v, a, sizeof(a), b, sizeof(b));
    lv_label_set_text(ip, a);
    lv_label_set_text(dp, b);
}

void ui_set_temperature(float celsius, bool valid)
{
    /* Colour follows the reading, so the gauge is legible at a glance even
     * before the number is read. Held at the cold end while invalid. */
    lv_color_t c = valid ? temp_color(celsius) : COL_T_COLD;
    lv_obj_set_style_arc_color(arc_temp, c, LV_PART_INDICATOR);

    /* The flame tracks the same ramp. Redrawn only when the colour actually
     * changes -- every redraw invalidates the icon and costs a flush. */
    if (!fire_col_valid || lv_color_to32(c) != lv_color_to32(fire_col_now)) {
        fire_col_now   = c;
        fire_col_valid = true;
        draw_icon(icon_fire_buf, icon_fire, FLAME_PTS,
                  (int)(sizeof(FLAME_PTS) / sizeof(FLAME_PTS[0])), c);
    }

    set_readout(lbl_ft_int, lbl_ft_dec, celsius, valid);
    set_gauge(arc_temp, lbl_temp, celsius, valid, TEMP_MIN, TEMP_MAX);
}

void ui_set_humidity(float percent, bool valid)
{
    set_readout(lbl_fh_int, lbl_fh_dec, percent, valid);
    set_gauge(arc_hum, lbl_hum, percent, valid, HUM_MIN, HUM_MAX);
}
