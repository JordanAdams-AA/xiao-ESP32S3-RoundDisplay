#pragma once

/* =====================================================================
 *  HARDWARE PINS  --  VERIFY against the Seeed wiki:
 *  "Getting Started with Seeed Studio Round Display for XIAO"
 *  D1..D10 are the XIAO silk-screen names, mapped by the board variant.
 *  These match the common Arduino_GFX setup for the XIAO round display,
 *  but the wiki is the source of truth if anything looks wrong.
 * ===================================================================== */
#define PIN_TFT_DC    D3     /* GC9A01 data/command            */
#define PIN_TFT_CS    D1     /* GC9A01 chip select             */
#define PIN_TFT_SCK   D8     /* SPI clock                      */
#define PIN_TFT_MOSI  D10    /* SPI data                       */
#define PIN_TFT_BL    D6     /* backlight (PWM). If dimming does nothing, this pin is wrong. */

#define SCREEN_W 240
#define SCREEN_H 240

/* =====================================================================
 *  MQTT TOPICS
 *  These are the Home Assistant mqtt_statestream topics for your
 *  temp/hum sensor. Change here if your entity or base_topic changes.
 * ===================================================================== */
#define TOPIC_TEMPERATURE "homeassistant/sensor/temphumzolder_temperature/state"
#define TOPIC_HUMIDITY    "homeassistant/sensor/temphumzolder_humidity/state"
#define TOPIC_LOG         "xiao/log"
#define TOPIC_STATUS      "xiao/status"

/* Gauge ranges (drive the arc fill; the real value is still shown as text) */
#define TEMP_MIN 0
#define TEMP_MAX 40
#define HUM_MIN  0
#define HUM_MAX  100

/* Timezone (Europe/Brussels, CET/CEST with DST rules) */
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

/* Night dimming */
#define DIM_START_HOUR 23    /* dim from 23:00 ... */
#define DIM_END_HOUR   6     /* ... until 06:00    */
#define BL_DAY   255         /* 0-255 backlight duty by day   */
#define BL_NIGHT 60          /* 0-255 backlight duty by night */

/* Set to 1 to show fixed dummy values without any MQTT (useful for a first
 * flash to check the face renders). MQTT still overrides if it connects. */
#define USE_DUMMY_DATA 0
#define DUMMY_TEMP 21.5f
#define DUMMY_HUM  48.0f
