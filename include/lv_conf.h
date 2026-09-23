/*
 * Minimal lv_conf.h for LVGL 8.3.x.
 * Only the settings this project needs are overridden here; everything else
 * falls back to LVGL's internal defaults (lv_conf_internal.h guards every macro).
 */
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ---- Colour ----
 * If the display shows wrong / byte-swapped colours (e.g. red<->blue looks off
 * or everything is tinted), flip this between 0 and 1. It also flips the draw
 * call in flush_cb() in main.cpp automatically. This is the #1 gotcha and the
 * one thing I could not verify without the hardware. */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 1

/* ---- Memory ---- */
#define LV_MEM_SIZE (64U * 1024U)

/* ---- Tick source: use Arduino millis(), no hardware timer needed ---- */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* ---- Drawing ---- */
#define LV_DRAW_COMPLEX 1          /* needed for rounded lines, arcs, etc. */

/* ---- Widgets used ---- */
#define LV_USE_CANVAS 1
#define LV_USE_ARC 1
#define LV_USE_LINE 1
#define LV_USE_LABEL 1
#define LV_USE_IMG 1

/* ---- Fonts used in the UI ---- */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1   /* climate page clock */
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* ---- Keep logging quiet ---- */
#define LV_USE_LOG 0

#endif /* LV_CONF_H */
