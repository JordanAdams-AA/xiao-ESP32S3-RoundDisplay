/*
 * XIAO ESP32-S3 + Round Display (GC9A01 240x240) analog watch face.
 * Temperature/humidity from Home Assistant over MQTT; time from NTP.
 * Web + serial-free debugging via MQTT logs; OTA via ElegantOTA.
 *
 * Threading model
 * ---------------
 * Rendering and networking run as two pinned FreeRTOS tasks:
 *
 *   ui_task  (core 1) -- LVGL, the clock, the backlight. Never blocks.
 *   net_task (core 0) -- Wi-Fi, MQTT, WebServer/OTA. Free to block.
 *
 * Core 0 is where the Wi-Fi driver already lives, so the blocking calls
 * (mqtt.connect(), DNS, scans) sit next to it and can no longer stall the
 * display. This is what removes the multi-second freezes of the second hand.
 *
 * Two libraries here are NOT thread-safe, so the boundary is strict:
 *   - LVGL: only ui_task may call lv_*() or ui_*(). net_task hands values
 *     over through `pending` under `pending_mux` instead of drawing.
 *   - PubSubClient: every use goes through `mqtt_mux`, because Wi-Fi events
 *     are delivered on a third task (the Arduino event task) that also logs.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <time.h>
#include <ctype.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_sntp.h"

#include "config.h"
#include "secrets.h"
#include "ui.h"
#include "touch.h"
#include "rtc.h"

#define UI_CORE   1
#define NET_CORE  0

/* Defined with the rest of the time handling further down, but needed by
 * update_clock() above it. */
static bool time_is_believable(void);

/* ---------------- Display ---------------- */
static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCK, PIN_TFT_MOSI, GFX_NOT_DEFINED);
static Arduino_GFX *gfx =
    new Arduino_GC9A01(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */);

/* ---------------- LVGL ---------------- */
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvbuf1;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
#if LV_COLOR_16_SWAP
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)color_p, w, h);
#endif
    lv_disp_flush_ready(drv);
}

/* ---------------- Net ---------------- */
static WiFiClient   net;
static PubSubClient mqtt(net);
static WebServer    server(80);

/* Recursive: mqtt_service() logs while already holding the lock. */
static SemaphoreHandle_t mqtt_mux;

static void logmsg(const char *m)
{
    Serial.println(m);
    if (xSemaphoreTakeRecursive(mqtt_mux, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (mqtt.connected()) mqtt.publish(TOPIC_LOG, m);
        xSemaphoreGiveRecursive(mqtt_mux);
    }
}

/* ---------------- net -> ui handover ----------------
 * The MQTT callback runs on net_task and must not touch LVGL, so it only
 * parks the value here. ui_task picks it up on its next pass and draws it. */
static SemaphoreHandle_t pending_mux;
static struct {
    float    temp,  hum;
    bool     temp_valid, hum_valid;
    bool     temp_dirty, hum_dirty;
    uint32_t temp_ms, hum_ms;     /* millis() when each was received */
    bool     link;                /* MQTT session is up */
} pending;

static void pending_set_link(bool up)
{
    if (xSemaphoreTake(pending_mux, pdMS_TO_TICKS(20)) != pdTRUE) return;
    pending.link = up;
    xSemaphoreGive(pending_mux);
}

static void pending_put(bool is_temp, float v, bool valid)
{
    if (xSemaphoreTake(pending_mux, pdMS_TO_TICKS(50)) != pdTRUE) return;
    uint32_t now = millis();
    if (is_temp) {
        pending.temp = v; pending.temp_valid = valid;
        pending.temp_dirty = true; pending.temp_ms = now;
    } else {
        pending.hum = v;  pending.hum_valid = valid;
        pending.hum_dirty = true;  pending.hum_ms = now;
    }
    xSemaphoreGive(pending_mux);
}

/* ui_task only. Copies out under the lock, then draws outside it.
 *
 * Also decides whether a reading is still worth showing. A value is only
 * displayed while the MQTT session is up and the value is recent: if the
 * broker goes away -- carrying the watch out of the house, say -- the
 * read-outs disappear rather than freezing on their last value, which would
 * otherwise sit there looking like a live reading. */
static bool climate_fresh(uint32_t now, uint32_t stamp)
{
    if (!stamp) return false;                    /* nothing received yet */
#if CLIMATE_STALE_SECONDS > 0
    return (now - stamp) < (uint32_t)CLIMATE_STALE_SECONDS * 1000UL;
#else
    return true;
#endif
}

static void pending_drain()
{
    static float    temp = 0, hum = 0;
    static uint32_t temp_ms = 0, hum_ms = 0;
    static bool     link = false;
    static bool     shown_t = false, shown_h = false, first = true;
    bool td = false, hd = false;

    if (xSemaphoreTake(pending_mux, 0) == pdTRUE) {
        td = pending.temp_dirty;
        hd = pending.hum_dirty;
        if (td) { temp = pending.temp; temp_ms = pending.temp_valid ? pending.temp_ms : 0; }
        if (hd) { hum  = pending.hum;  hum_ms  = pending.hum_valid  ? pending.hum_ms  : 0; }
        pending.temp_dirty = pending.hum_dirty = false;
        link = pending.link;
        xSemaphoreGive(pending_mux);
    }

    uint32_t now = millis();
#if USE_DUMMY_DATA
    bool ok_t = true, ok_h = true;               /* bench mode: always show */
#else
    bool ok_t = link && climate_fresh(now, temp_ms);
    bool ok_h = link && climate_fresh(now, hum_ms);
#endif

    /* Redraw on a new value, on a visibility change, or once at startup. */
    if (first || ok_t != shown_t || (ok_t && td)) {
        ui_set_temperature(temp, ok_t);
        shown_t = ok_t;
    }
    if (first || ok_h != shown_h || (ok_h && hd)) {
        ui_set_humidity(hum, ok_h);
        shown_h = ok_h;
    }
    first = false;
}

/* ---------------- Backlight (core 2.x / 3.x compatible) ---------------- */
static void backlight_init()
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(PIN_TFT_BL, 5000, 8);
    ledcWrite(PIN_TFT_BL, BL_DAY);
#else
    ledcSetup(0, 5000, 8);
    ledcAttachPin(PIN_TFT_BL, 0);
    ledcWrite(0, BL_DAY);
#endif
}
static void backlight_set(uint8_t duty)
{
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(PIN_TFT_BL, duty);
#else
    ledcWrite(0, duty);
#endif
}

/* ---------------- WiFi ----------------
 * setup() used to wait 15 s and then never look at Wi-Fi again, so a slow or
 * momentarily-failed association meant the device stayed offline forever.
 * These keep retrying and report exactly why an attempt failed. */
static void wifi_event(WiFiEvent_t ev, WiFiEventInfo_t info)
{
    char m[128];
    switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        logmsg("wifi: associated, waiting for IP");
        break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        /* RSSI guide: > -60 good, -70 usable, < -80 marginal. A persistently
         * low value here means the U.FL antenna is missing, unseated, or
         * shadowed by the display board -- not something firmware can fix. */
        snprintf(m, sizeof(m), "wifi: got ip=%s rssi=%d dBm ch=%d txpower=%d",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel(),
                 (int)WiFi.getTxPower());
        logmsg(m);
        break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        /* reason 2/15/205 = auth/handshake (wrong password), 201 = AP not found */
        snprintf(m, sizeof(m), "wifi: disconnected, reason=%d",
                 info.wifi_sta_disconnected.reason);
        logmsg(m);
        break;
    default:
        break;
    }
}

static void wifi_start()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);          /* sleep can stall association on some APs */
    /* This SSID is served by more than one AP. The default fast scan stops at
     * the first match, which repeatedly picked a -88 dBm radio while a -53 dBm
     * one was in range. Scan every channel and take the strongest. */
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    /* Ask for full transmit power explicitly rather than trusting the default.
     * Worth doing on this board: the XIAO S3 has no usable on-board antenna,
     * so link margin is tight and every dB counts. */
    WiFi.setTxPower(WIFI_POWER_19_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void wifi_service()
{
    if (WiFi.status() == WL_CONNECTED) return;

    static uint32_t last = 0;
    if (millis() - last < 10000) return;   /* retry every 10 s */
    last = millis();

    char m[96];
    snprintf(m, sizeof(m), "wifi: not connected (status=%d), retrying",
             (int)WiFi.status());
    logmsg(m);
    WiFi.disconnect(true);
    wifi_start();
}

/* ---------------- MQTT ---------------- */
static bool payload_is_number(const char *s)
{
    if (!s || !*s) return false;
    return isdigit((unsigned char)s[0]) || s[0] == '-' || s[0] == '+' || s[0] == '.';
}

static void mqtt_cb(char *topic, byte *payload, unsigned int len)
{
    char buf[32];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, payload, len);
    buf[len] = '\0';

    bool valid = payload_is_number(buf);
    float v = atof(buf);

    /* net_task context: park the value, ui_task draws it. */
    if (strcmp(topic, TOPIC_TEMPERATURE) == 0) {
        pending_put(true, v, valid);
    } else if (strcmp(topic, TOPIC_HUMIDITY) == 0) {
        pending_put(false, v, valid);
    }
}

static void mqtt_service()
{
    if (WiFi.status() != WL_CONNECTED) return;

    if (xSemaphoreTakeRecursive(mqtt_mux, pdMS_TO_TICKS(200)) != pdTRUE) return;

    if (mqtt.connected()) {
        mqtt.loop();
        xSemaphoreGiveRecursive(mqtt_mux);
        return;
    }

    static uint32_t last = 0;
    if (millis() - last < 3000) {          /* backoff between connect attempts */
        xSemaphoreGiveRecursive(mqtt_mux);
        return;
    }
    last = millis();

    /* Blocking, but this is core 0 now -- the UI keeps rendering throughout.
     * The timeouts above bound it well inside the task-watchdog window; an
     * unreachable broker over a weak link would otherwise sit here long
     * enough to starve the core 0 idle task and panic the device. */
    uint32_t t_connect = millis();
    Serial.printf("mqtt: connecting to %s:%d ...\n", MQTT_HOST, MQTT_PORT);
    bool ok = mqtt.connect(HOSTNAME, MQTT_USER, MQTT_PASS, TOPIC_STATUS, 0, true, "offline");
    Serial.printf("mqtt: connect %s after %lums (state=%d)\n",
                  ok ? "OK" : "FAILED",
                  (unsigned long)(millis() - t_connect), mqtt.state());
    if (ok) {
        mqtt.publish(TOPIC_STATUS, "online", true);
        mqtt.subscribe(TOPIC_TEMPERATURE);
        mqtt.subscribe(TOPIC_HUMIDITY);
        char m[96];
        snprintf(m, sizeof(m), "mqtt connected, ip=%s rssi=%d heap=%u",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                 (unsigned)ESP.getFreeHeap());
        logmsg(m);
    }
    xSemaphoreGiveRecursive(mqtt_mux);
}

/* ---------------- HTTP status page + OTA ---------------- */
static void handle_root()
{
    char html[512];
    snprintf(html, sizeof(html),
             "<html><body style='font-family:sans-serif'>"
             "<h2>XIAO watch face</h2>"
             "<p>IP: %s<br>RSSI: %d dBm<br>Free heap: %u<br>Uptime: %lus</p>"
             "<p><a href='/update'>Firmware update (OTA)</a></p>"
             "</body></html>",
             WiFi.localIP().toString().c_str(), WiFi.RSSI(),
             (unsigned)ESP.getFreeHeap(), millis() / 1000);
    server.send(200, "text/html", html);
}

/* ---------------- Time (ui_task only) ---------------- */
static void update_clock()
{
    /* Read the clock without blocking. getLocalTime() sleeps internally while
     * the year is still 1970, which would stall the render loop every pass. */
    time_t now = time(nullptr);
    struct tm ti;
    localtime_r(&now, &ti);
    /* Believable means seeded from the RTC or corrected by NTP; an unset
     * clock reads 1970 and must not be shown as if it were the time. */
    bool synced = time_is_believable();

    /* One discrete tick per second. Redrawing only on a second boundary is
     * both the look that was asked for and far less work than the old ~20 Hz
     * interpolated sweep, which is what made the motion look uneven. */
    static int lastSec = -1;
    if (ti.tm_sec == lastSec) return;
    lastSec = ti.tm_sec;

    ui_set_time(ti.tm_hour, ti.tm_min, (float)ti.tm_sec,
                ti.tm_wday, ti.tm_mday, ti.tm_mon, synced);

    /* Night dimming */
    static int lastDim = -1;
    bool night = (DIM_START_HOUR < DIM_END_HOUR)
                 ? (ti.tm_hour >= DIM_START_HOUR && ti.tm_hour < DIM_END_HOUR)
                 : (ti.tm_hour >= DIM_START_HOUR || ti.tm_hour < DIM_END_HOUR);
    int want = night ? BL_NIGHT : BL_DAY;
    if (want != lastDim) { backlight_set((uint8_t)want); lastDim = want; }
}

static Preferences prefs;

/* ---------------- Time ----------------
 * The RTC is the clock of record. It is read once at boot, so the watch knows
 * the time with no network at all, and the system clock is realigned to it
 * periodically. NTP only corrects: each successful sync is written back to
 * the RTC, which is also what repairs a drifting or freshly-batteried chip.
 *
 * The RTC always holds UTC. Local time is applied through the TZ offset, so
 * changing time zone never rewrites the chip.
 *
 * All RTC access happens on ui_task, which already owns the I2C bus for the
 * touch controller -- that avoids a second lock on a shared bus. */
static int  utc_offset   = DEFAULT_UTC_OFFSET_HOURS;
static volatile bool ntp_fresh = false;   /* a sync landed, write it to RTC */

/* Anything older than this is not a real time; used to decide whether the
 * clock is believable and what a manual adjustment should start from. */
#define TIME_SANE_EPOCH 1767225600L       /* 2026-01-01T00:00:00Z */

/* timegm() is not exposed by this newlib build, and mktime() would apply the
 * local offset -- wrong for a chip that stores UTC. Days-from-civil instead
 * (Howard Hinnant's algorithm), which is exact and has no timezone state. */
static time_t utc_from_tm(const struct tm *t)
{
    int      y = t->tm_year + 1900;
    unsigned m = (unsigned)t->tm_mon + 1;
    unsigned d = (unsigned)t->tm_mday;
    y -= (m <= 2);
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    long days = (long)era * 146097L + (long)doe - 719468L;
    return (time_t)days * 86400L
         + (time_t)t->tm_hour * 3600 + (time_t)t->tm_min * 60 + t->tm_sec;
}

static bool time_is_believable(void)
{
    return time(nullptr) > TIME_SANE_EPOCH;
}

static void apply_timezone(int off)
{
    if (off < UTC_OFFSET_MIN) off = UTC_OFFSET_MIN;
    if (off > UTC_OFFSET_MAX) off = UTC_OFFSET_MAX;
    utc_offset = off;

    /* POSIX TZ counts the other way round: UTC+1 is written "UTC-1". */
    char tz[16];
    snprintf(tz, sizeof(tz), "UTC%+d", -off);
    setenv("TZ", tz, 1);
    tzset();

    prefs.begin("watchface", false);
    if (prefs.getChar("tz", 127) != (int8_t)off) prefs.putChar("tz", (int8_t)off);
    prefs.end();

    Serial.printf("time: offset UTC%+d (TZ=%s)\n", off, tz);
}

/* SNTP task context: do nothing here but raise a flag. */
static void on_ntp_sync(struct timeval *tv)
{
    LV_UNUSED(tv);
    ntp_fresh = true;
}

/* ui_task only: push the freshly synced system clock into the RTC. */
static void rtc_store_now(const char *why)
{
    if (!rtc_present()) return;
    time_t now = time(nullptr);
    if (now <= TIME_SANE_EPOCH) return;
    struct tm utc;
    gmtime_r(&now, &utc);
    if (rtc_write(&utc))
        Serial.printf("rtc: written from %s (%04d-%02d-%02d %02d:%02d:%02d UTC)\n",
                      why, utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                      utc.tm_hour, utc.tm_min, utc.tm_sec);
    else
        Serial.println("rtc: write FAILED");
}

/* Shift the clock by whole minutes and persist it. Starts from a sane date
 * when the clock has never been set, so the arrows are usable on a board
 * with no network and no RTC battery. */
static void time_adjust(int delta_minutes)
{
    time_t now = time(nullptr);
    if (now <= TIME_SANE_EPOCH) now = TIME_SANE_EPOCH;
    now += (time_t)delta_minutes * 60;

    struct timeval tv = { .tv_sec = now, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    rtc_store_now("manual set");
}

static void tz_adjust(int delta_hours)
{
    apply_timezone(utc_offset + delta_hours);
    ui_set_tz_offset(utc_offset);
}

/* ---------------- Rotation ----------------
 * Turning the panel is done in the GC9A01's scan mapping, not by rotating
 * pixels in software: it costs nothing per frame. The screen is square, so
 * LVGL's resolution is unchanged. The choice is kept in NVS because it
 * describes how the device is physically mounted. */
static void apply_rotation(uint8_t r)
{
    gfx->setRotation(r);
    touch_set_rotation(r);          /* keep taps and swipes aligned */
    gfx->fillScreen(BLACK);         /* drop whatever the old mapping left */

    /* Only write on a real change: this also runs when restoring at boot,
     * and rewriting the same value every time is pointless flash wear. */
    prefs.begin("watchface", false);
    if (prefs.getUChar("rot", 0) != r) prefs.putUChar("rot", r);
    prefs.end();

    Serial.printf("ui: rotation set to %d deg\n", r * 90);
}

/* ---------------- LVGL input device ----------------
 * Needed for the settings page button; the touch driver's own gesture
 * detection still drives paging. */
static void lvgl_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    LV_UNUSED(drv);
    int x = 0, y = 0;
    if (touch_get_state(&x, &y)) {
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* ---------------- Paging (ui_task only) ----------------
 * The CST816 reports swipes itself, so there is no LVGL input device and no
 * dependence on the touch panel's axes matching the display rotation. */
static void service_pages()
{
    if (!touch_present()) return;

    TouchGesture g = touch_poll();
    if (g == TG_NONE) return;

    Serial.printf("touch: gesture=%d travel=%u page=%d\n",
                  (int)g, (unsigned)touch_last_raw(), ui_page_get());

    /* The content follows the finger, so the tile that comes into view is
     * the one on that side: swipe up reveals the tile below. */
    const int s = SWIPE_INVERT ? -1 : 1;
    switch (g) {
    case TG_LEFT:  ui_nav( 1 * s,  0); break;
    case TG_RIGHT: ui_nav(-1 * s,  0); break;
    case TG_UP:    ui_nav( 0,  1 * s); break;   /* settings sits below */
    case TG_DOWN:  ui_nav( 0, -1 * s); break;
    default:       break;
    }
}

/* ui_task only: owns the I2C bus, so all RTC traffic lives here. */
static void service_rtc(void)
{
    if (ntp_fresh) {
        ntp_fresh = false;
        rtc_store_now("ntp");
        return;
    }

    /* Between NTP syncs, realign the system clock to the RTC: the chip's
     * watch crystal holds time better than the ESP32 running free, and this
     * is what makes the RTC the authority rather than a mere backup. */
    static uint32_t last = 0;
    if (!rtc_present()) return;
    if (millis() - last < (uint32_t)RTC_RESYNC_SECONDS * 1000UL) return;
    last = millis();

    struct tm utc;
    bool valid = false;
    if (!rtc_read(&utc, &valid) || !valid) return;

    time_t t = utc_from_tm(&utc);
    if (t <= TIME_SANE_EPOCH) return;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/* ==================================================================== */
/* Core 1: rendering only. Nothing in here is allowed to block.         */
static void ui_task(void *)
{
#if PERF_LOG_SECONDS > 0
    uint32_t worst = 0, total = 0, calls = 0, next_report = 0;
#endif
    for (;;) {
#if PERF_LOG_SECONDS > 0
        uint32_t t0 = millis();
        lv_timer_handler();
        uint32_t dt = millis() - t0;
        if (dt > worst) worst = dt;
        total += dt;
        calls++;
        if (millis() > next_report) {
            next_report = millis() + (uint32_t)PERF_LOG_SECONDS * 1000;
            /* Worst case is the number that matters: it is how long touch
             * sampling and the clock stall while a repaint is in flight. */
            Serial.printf("perf: lv_timer_handler avg=%lums worst=%lums over "
                          "%lu calls\n",
                          (unsigned long)(calls ? total / calls : 0),
                          (unsigned long)worst, (unsigned long)calls);
            worst = total = calls = 0;
        }
#else
        lv_timer_handler();
#endif
        pending_drain();
        update_clock();
        service_pages();
        service_rtc();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* Core 0: everything that may block, next to the Wi-Fi driver.         */
static void net_task(void *)
{
    WiFi.onEvent(wifi_event);
    wifi_start();

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
        vTaskDelay(pdMS_TO_TICKS(100));   /* UI is on the other core, unaffected */

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi: connected in %lums, ip=%s\n",
                      (unsigned long)(millis() - t0),
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.printf("wifi: NOT connected after 15s (status=%d). "
                      "Scanning for the configured SSID...\n", (int)WiFi.status());
        int n = WiFi.scanNetworks();
        bool found = false;
        for (int i = 0; i < n; i++) {
            if (WiFi.SSID(i) == String(WIFI_SSID)) {
                found = true;
                Serial.printf("  FOUND \"%s\" rssi=%d ch=%d enc=%d\n",
                              WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                              WiFi.channel(i), (int)WiFi.encryptionType(i));
            }
        }
        if (!found)
            Serial.printf("  SSID \"%s\" NOT visible (%d networks seen). "
                          "Check spelling/case, or it is 5GHz-only.\n", WIFI_SSID, n);
        WiFi.scanDelete();
        wifi_start();
    }

    /* Ask for UTC (offset 0) and apply the local offset through TZ instead,
     * so the time zone can change at runtime without restarting SNTP. */
    sntp_set_time_sync_notification_cb(on_ntp_sync);
    sntp_set_sync_interval((uint32_t)NTP_SYNC_INTERVAL_SECONDS * 1000UL);
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    apply_timezone(utc_offset);

    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqtt_cb);
    mqtt.setBufferSize(512);
    /* Bound how long a single broker attempt can block this task. Without
     * these, an unreachable broker blocks for the stack default (tens of
     * seconds), which trips the task watchdog. WiFiClient::setTimeout()
     * takes SECONDS on the ESP32 core, not milliseconds. */
    net.setTimeout(3);
    mqtt.setSocketTimeout(3);

    server.on("/", handle_root);
    ElegantOTA.begin(&server);
    ElegantOTA.setAuth("admin", OTA_PASSWORD);
    server.begin();

    for (;;) {
        wifi_service();
        server.handleClient();
        ElegantOTA.loop();
        mqtt_service();
        /* Drives whether the climate read-outs are shown at all. */
        pending_set_link(WiFi.status() == WL_CONNECTED && mqtt.connected());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup()
{
    Serial.begin(115200);
    /* USB CDC enumerates after boot; give the host a moment so the first
     * diagnostics are not lost. Bounded so it still runs headless. */
    for (uint32_t t = millis(); !Serial && millis() - t < 2000; ) delay(10);
    Serial.println();
    Serial.printf("xiao-watchface boot, ssid=\"%s\" core=%d.%d.%d\n",
                  WIFI_SSID, ESP_ARDUINO_VERSION_MAJOR,
                  ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);

    mqtt_mux    = xSemaphoreCreateRecursiveMutex();
    pending_mux = xSemaphoreCreateMutex();

    /* net_task deliberately makes blocking socket calls on core 0, so the
     * core 0 idle task can go unscheduled for longer than the 5 s default.
     * Each individual call is bounded to ~3 s (see net.setTimeout below),
     * so 15 s leaves headroom while still catching a genuine hang. Panic is
     * kept on: a reboot is the right response to a wedged network task. */
    esp_task_wdt_init(15, true);

    backlight_init();
    gfx->begin(GFX_SPI_HZ);
    gfx->fillScreen(BLACK);

    lv_init();

    size_t bufsz = SCREEN_W * LVBUF_LINES * sizeof(lv_color_t);
    lvbuf1 = (lv_color_t *)heap_caps_malloc(bufsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    bool buf_internal = (lvbuf1 != NULL);
    if (!lvbuf1) lvbuf1 = (lv_color_t *)heap_caps_malloc(bufsz, MALLOC_CAP_SPIRAM);
    Serial.printf("lvgl: draw buffer %u bytes (%u lines) in %s\n",
                  (unsigned)bufsz, (unsigned)LVBUF_LINES,
                  buf_internal ? "internal DMA RAM" : "PSRAM");
    lv_disp_draw_buf_init(&draw_buf, lvbuf1, NULL, SCREEN_W * LVBUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_W;
    disp_drv.ver_res  = SCREEN_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* Probe touch before the tasks start so the result is logged in order. */
    touch_init();

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_read;
    lv_indev_drv_register(&indev_drv);

    ui_create();

    /* Restore the mounting orientation chosen on the settings page. */
    prefs.begin("watchface", true);
    uint8_t rot = prefs.getUChar("rot", 0);
    int8_t  tz  = prefs.getChar("tz", (int8_t)DEFAULT_UTC_OFFSET_HOURS);
    prefs.end();
    ui_set_rotate_handler(apply_rotation);
    ui_set_rotation(rot);

    ui_set_time_adjust_handler(time_adjust);
    ui_set_tz_adjust_handler(tz_adjust);
    apply_timezone(tz);
    ui_set_tz_offset(utc_offset);

    /* Seed the system clock from the RTC before anything reads it, so the
     * face shows a real time immediately even with no network. */
    if (rtc_init()) {
        struct tm utc;
        bool valid = false;
        if (rtc_read(&utc, &valid)) {
            if (valid) {
                time_t t = utc_from_tm(&utc);
                if (t > TIME_SANE_EPOCH) {
                    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                    settimeofday(&tv, NULL);
                    Serial.printf("rtc: seeded clock %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                                  utc.tm_hour, utc.tm_min, utc.tm_sec);
                }
            } else {
                Serial.println("rtc: voltage-low flag set -- time not trusted "
                               "(no backup cell yet?)");
            }
        }
    }

#if USE_DUMMY_DATA
    ui_set_temperature(DUMMY_TEMP, true);   /* safe: no tasks running yet */
    ui_set_humidity(DUMMY_HUM, true);
#endif

    /* UI first so the face is live before the network bring-up starts. */
    xTaskCreatePinnedToCore(ui_task,  "ui",  8192, NULL, 3, NULL, UI_CORE);
    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 2, NULL, NET_CORE);
}

void loop()
{
    /* Both jobs live in their own pinned tasks now. Keep the Arduino loop
     * task idle rather than deleting it; ElegantOTA and WebServer expect the
     * scheduler to stay in its normal shape. */
    vTaskDelay(pdMS_TO_TICKS(1000));
}
