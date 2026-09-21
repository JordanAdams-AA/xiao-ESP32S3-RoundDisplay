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
#include <time.h>
#include <ctype.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

#include "config.h"
#include "secrets.h"
#include "ui.h"

#define UI_CORE   1
#define NET_CORE  0

/* ---------------- Display ---------------- */
static Arduino_DataBus *bus =
    new Arduino_ESP32SPI(PIN_TFT_DC, PIN_TFT_CS, PIN_TFT_SCK, PIN_TFT_MOSI, GFX_NOT_DEFINED);
static Arduino_GFX *gfx =
    new Arduino_GC9A01(bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, true /* IPS */);

/* ---------------- LVGL ---------------- */
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvbuf1;
static const uint32_t LVBUF_LINES = 40;   /* partial buffer, 40 lines tall */

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
    float temp,  hum;
    bool  temp_valid, hum_valid;
    bool  temp_dirty, hum_dirty;
} pending;

static void pending_put(bool is_temp, float v, bool valid)
{
    if (xSemaphoreTake(pending_mux, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (is_temp) { pending.temp = v; pending.temp_valid = valid; pending.temp_dirty = true; }
    else         { pending.hum  = v; pending.hum_valid  = valid; pending.hum_dirty  = true; }
    xSemaphoreGive(pending_mux);
}

/* ui_task only. Copies out under the lock, then draws outside it. */
static void pending_drain()
{
    float t = 0, h = 0;
    bool tv = false, hv = false, td = false, hd = false;

    if (xSemaphoreTake(pending_mux, 0) != pdTRUE) return;   /* try again next pass */
    td = pending.temp_dirty; t = pending.temp; tv = pending.temp_valid;
    hd = pending.hum_dirty;  h = pending.hum;  hv = pending.hum_valid;
    pending.temp_dirty = pending.hum_dirty = false;
    xSemaphoreGive(pending_mux);

    if (td) ui_set_temperature(t, tv);
    if (hd) ui_set_humidity(h, hv);
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
    bool synced = (ti.tm_year > (2016 - 1900));

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

/* ==================================================================== */
/* Core 1: rendering only. Nothing in here is allowed to block.         */
static void ui_task(void *)
{
    for (;;) {
        lv_timer_handler();
        pending_drain();
        update_clock();
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

    configTzTime(TZ_INFO, "pool.ntp.org", "time.cloudflare.com");

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
    gfx->begin();
    gfx->fillScreen(BLACK);

    lv_init();

    size_t bufsz = SCREEN_W * LVBUF_LINES * sizeof(lv_color_t);
    lvbuf1 = (lv_color_t *)heap_caps_malloc(bufsz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!lvbuf1) lvbuf1 = (lv_color_t *)heap_caps_malloc(bufsz, MALLOC_CAP_SPIRAM);
    lv_disp_draw_buf_init(&draw_buf, lvbuf1, NULL, SCREEN_W * LVBUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_W;
    disp_drv.ver_res  = SCREEN_H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    ui_create();

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
