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

/* Capacitive touch (CHSC6X) on the Round Display's I2C bus.
 * D4/D5 are the XIAO's hardware I2C pins (GPIO5/GPIO6).
 *
 * The controller only acknowledges I2C while a finger is actually down, so
 * it cannot be found by a bus scan. It raises PIN_TOUCH_INT low instead, and
 * that is what the driver watches. The PCF8563 RTC at 0x51 is the reliable
 * proof that the display board is attached at all. */
#define PIN_TOUCH_SDA   D4
#define PIN_TOUCH_SCL   D5
#define PIN_TOUCH_INT   D7
#define TOUCH_I2C_ADDR  0x2E
#define RTC_I2C_ADDR    0x51

/* Minimum horizontal travel (pixels) before a drag counts as a swipe. */
#define SWIPE_MIN_PX 40

/* Set to 1 if a swipe moves the pages the wrong way. The touch panel's axes
 * are not guaranteed to line up with the display's rotation. */
#define SWIPE_INVERT 0

/* Fallback when no touch controller is detected: cycle the pages on a timer
 * so the second page is still reachable. 0 disables it (stays on page 1). */
#define AUTO_PAGE_SECONDS 20

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
