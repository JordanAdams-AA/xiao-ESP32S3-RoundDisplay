#pragma once
#include <stdbool.h>
#include <time.h>

/* PCF8563 real-time clock on the Round Display board (I2C 0x51).
 *
 * This is the clock of record. It keeps running on its backup cell when the
 * board is unpowered, so the watch knows the time at boot without a network.
 * NTP only corrects it; see main.cpp.
 *
 * Everything here works in UTC. Local time is a display concern, applied
 * through the TZ offset, so changing time zone never rewrites the RTC.
 *
 * All calls use the shared I2C bus and must come from the UI task only,
 * which is the task that also drives the touch controller. */
bool rtc_init(void);
bool rtc_present(void);

/* Read UTC. Returns false on a bus error. *valid is false when the chip's
 * voltage-low flag is set, meaning the oscillator stopped and whatever it
 * reports is not to be trusted -- a dead or missing backup cell. */
bool rtc_read(struct tm *out, bool *valid);

/* Write UTC and clear the voltage-low flag. */
bool rtc_write(const struct tm *t);
