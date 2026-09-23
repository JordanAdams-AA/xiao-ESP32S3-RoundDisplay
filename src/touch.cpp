#include "touch.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>

/* CHSC6X on the Seeed Round Display.
 *
 * Two properties of this controller shape the driver:
 *   - No gesture register, so swipes are reconstructed from the start and end
 *     coordinates of each touch.
 *   - It only acknowledges I2C while a finger is down, so it cannot be found
 *     by a bus scan; the interrupt line on D7 is what announces a touch.
 *
 * INT is latched in an ISR rather than sampled. Polling it directly missed
 * most swipes: the line can pulse briefly on data-ready instead of staying
 * low for the whole touch, and a poll every few milliseconds -- with gaps of
 * tens of milliseconds whenever LVGL is mid-redraw -- simply walked past it.
 * An edge cannot be missed this way even while the UI task is busy. */
#define CHSC6X_POINT_LEN 5

/* A touch is finished once this long passes with no further INT activity. */
#define TOUCH_IDLE_MS 90

static bool    present  = false;
static uint8_t last_raw = 0;

static volatile bool irq_hit = false;

static bool     tracking = false;
static int      start_x = 0, start_y = 0;
static int      cur_x   = 0, cur_y   = 0;
static uint32_t last_ms = 0;
static uint8_t  rotation = 0;

static void IRAM_ATTR touch_isr(void)
{
    irq_hit = true;
}

/* Map raw panel coordinates into the rotated screen space the user sees.
 * Doing it here means gestures fall out correctly too: dx/dy are already in
 * display space, so "up" is whichever way is up in the current rotation. */
static void apply_rotation(int rx, int ry, int *lx, int *ly)
{
    /* Cases 1 and 3 are each other's inverse, and they were the wrong way
     * round: at 90 and 270 degrees swipes came out reversed while 0 and 180
     * behaved. 180 hid the error because it is its own inverse -- applying a
     * backwards 90 twice gives -180, which is the same mapping as +180. */
    switch (rotation & 3) {
    case 1:  *lx = ry;                *ly = SCREEN_H - 1 - rx;    break;
    case 2:  *lx = SCREEN_W - 1 - rx; *ly = SCREEN_H - 1 - ry;    break;
    case 3:  *lx = SCREEN_W - 1 - ry; *ly = rx;                   break;
    default: *lx = rx;                *ly = ry;                   break;
    }
}

static bool read_point(int *x, int *y)
{
    uint8_t b[CHSC6X_POINT_LEN] = {0};
    if (Wire.requestFrom((uint8_t)TOUCH_I2C_ADDR, (uint8_t)CHSC6X_POINT_LEN)
        != CHSC6X_POINT_LEN)
        return false;
    for (int i = 0; i < CHSC6X_POINT_LEN; i++) b[i] = Wire.read();
    /* b[0] is a status/'points present' marker; coordinates are single bytes,
     * which is why this panel tops out at 255 and suits a 240px screen. */
    if (b[0] == 0) return false;
    apply_rotation(b[2], b[4], x, y);
    return true;
}

void touch_set_rotation(uint8_t r) { rotation = r & 3; }

bool touch_get_state(int *x, int *y)
{
    if (!tracking) return false;
    /* The finger is considered down until the INT line has been quiet for
     * TOUCH_IDLE_MS; touch_poll() is what ends the press. */
    *x = cur_x;
    *y = cur_y;
    return true;
}

bool touch_init(void)
{
    Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    Wire.setClock(400000);
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);

    /* The touch chip is silent at rest, so presence is inferred from the RTC
     * on the same board: if it answers, the display board is attached and the
     * touch panel should be there too. */
    Wire.beginTransmission(RTC_I2C_ADDR);
    bool board = (Wire.endTransmission(true) == 0);

    present = board;
    if (present) {
        attachInterrupt(digitalPinToInterrupt(PIN_TOUCH_INT), touch_isr, FALLING);
        Serial.printf("touch: display board detected (RTC 0x%02X); "
                      "CHSC6X addr 0x%02X, INT on GPIO%d (edge latched)\n",
                      RTC_I2C_ADDR, TOUCH_I2C_ADDR, PIN_TOUCH_INT);
    } else {
        Serial.printf("touch: no device on SDA=%d SCL=%d -- display board's "
                      "I2C side not connected. Swipe disabled.\n",
                      PIN_TOUCH_SDA, PIN_TOUCH_SCL);
    }
    return present;
}

bool touch_present(void) { return present; }
uint8_t touch_last_raw(void) { return last_raw; }

TouchGesture touch_poll(void)
{
    if (!present) return TG_NONE;

    /* Either a latched edge or a still-asserted line means "finger down".
     * Handling both covers a pulsed INT and a level-held INT alike. */
    bool active = irq_hit || (digitalRead(PIN_TOUCH_INT) == LOW);
    if (active) {
        irq_hit = false;
        int x, y;
        if (read_point(&x, &y)) {
            if (!tracking) {
                tracking = true;
                start_x = x;
                start_y = y;
            }
            cur_x = x;
            cur_y = y;
            last_ms = millis();
        }
        return TG_NONE;                 /* decide on release */
    }

    if (!tracking) return TG_NONE;
    if (millis() - last_ms < TOUCH_IDLE_MS) return TG_NONE;   /* still down */
    tracking = false;

    int dx = cur_x - start_x;
    int dy = cur_y - start_y;
    int ax = abs(dx), ay = abs(dy);
    last_raw = (uint8_t)(ax > ay ? ax : ay);

    if (ax >= SWIPE_MIN_PX && ax > ay) return dx < 0 ? TG_LEFT : TG_RIGHT;
    if (ay >= SWIPE_MIN_PX && ay > ax) return dy < 0 ? TG_UP   : TG_DOWN;
    if (ax < 12 && ay < 12)            return TG_TAP;
    return TG_NONE;
}
