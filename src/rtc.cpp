#include "rtc.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>

/* PCF8563 register map */
#define REG_CTRL1      0x00
#define REG_CTRL2      0x01
#define REG_VL_SECONDS 0x02   /* bit 7 = VL, oscillator stopped since last read */
#define REG_MINUTES    0x03
#define REG_HOURS      0x04
#define REG_DAYS       0x05
#define REG_WEEKDAYS   0x06
#define REG_CENT_MONTH 0x07   /* bit 7 = century */
#define REG_YEARS      0x08

static bool present = false;

static uint8_t bcd2dec(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t dec2bcd(uint8_t d) { return (uint8_t)(((d / 10) << 4) | (d % 10)); }

bool rtc_init(void)
{
    /* Wire is already up: touch_init() owns the bus setup. */
    Wire.beginTransmission(RTC_I2C_ADDR);
    present = (Wire.endTransmission(true) == 0);
    if (!present) {
        Serial.printf("rtc: no PCF8563 at 0x%02X\n", RTC_I2C_ADDR);
        return false;
    }
    Serial.printf("rtc: PCF8563 found at 0x%02X\n", RTC_I2C_ADDR);
    return true;
}

bool rtc_present(void) { return present; }

bool rtc_read(struct tm *out, bool *valid)
{
    if (!present || !out) return false;

    Wire.beginTransmission(RTC_I2C_ADDR);
    Wire.write(REG_VL_SECONDS);
    if (Wire.endTransmission(true) != 0) return false;
    if (Wire.requestFrom((uint8_t)RTC_I2C_ADDR, (uint8_t)7) != 7) return false;

    uint8_t b[7];
    for (int i = 0; i < 7; i++) b[i] = Wire.read();

    if (valid) *valid = ((b[0] & 0x80) == 0);   /* VL set => time unreliable */

    out->tm_sec  = bcd2dec(b[0] & 0x7F);
    out->tm_min  = bcd2dec(b[1] & 0x7F);
    out->tm_hour = bcd2dec(b[2] & 0x3F);
    out->tm_mday = bcd2dec(b[3] & 0x3F);
    out->tm_wday = b[4] & 0x07;
    out->tm_mon  = bcd2dec(b[5] & 0x1F) - 1;    /* chip counts months from 1 */
    /* Century bit: 0 = 20xx, 1 = 19xx. Nothing here predates 2000. */
    out->tm_year = bcd2dec(b[6]) + ((b[5] & 0x80) ? 0 : 100);
    out->tm_isdst = 0;

    /* Guard against a chip that answers but returns nonsense. */
    if (out->tm_mon < 0 || out->tm_mon > 11 || out->tm_mday < 1 ||
        out->tm_mday > 31 || out->tm_hour > 23 || out->tm_min > 59 ||
        out->tm_sec > 59) {
        if (valid) *valid = false;
    }
    return true;
}

bool rtc_write(const struct tm *t)
{
    if (!present || !t) return false;

    int year = t->tm_year - 100;                /* tm_year is from 1900 */
    if (year < 0 || year > 99) return false;

    Wire.beginTransmission(RTC_I2C_ADDR);
    Wire.write(REG_VL_SECONDS);
    Wire.write(dec2bcd((uint8_t)t->tm_sec) & 0x7F);   /* clears VL */
    Wire.write(dec2bcd((uint8_t)t->tm_min));
    Wire.write(dec2bcd((uint8_t)t->tm_hour));
    Wire.write(dec2bcd((uint8_t)t->tm_mday));
    Wire.write((uint8_t)(t->tm_wday & 0x07));
    Wire.write((uint8_t)(dec2bcd((uint8_t)(t->tm_mon + 1)) & 0x7F));  /* century 0 */
    Wire.write(dec2bcd((uint8_t)year));
    return Wire.endTransmission(true) == 0;
}
