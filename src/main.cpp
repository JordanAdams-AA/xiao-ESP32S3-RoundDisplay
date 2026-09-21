/*
 * XIAO ESP32-S3 + Round Display (GC9A01 240x240) analog watch face.
 * Temperature/humidity from Home Assistant over MQTT; time from NTP.
 * Web + serial-free debugging via MQTT logs; OTA via ElegantOTA.
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

#include "config.h"
#include "secrets.h"
#include "ui.h"

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

static void logmsg(const char *m)
{
    Serial.println(m);
    if (mqtt.connected()) mqtt.publish(TOPIC_LOG, m);
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
        snprintf(m, sizeof(m), "wifi: got ip=%s rssi=%d ch=%d",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
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
    WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void wifi_service()
{
    if (WiFi.status() == WL_CONNECTED) return;

    static uint32_t last = 0;
    if (millis() - last < 10000) return;   /* retry every 10 s, non-blocking */
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

    if (strcmp(topic, TOPIC_TEMPERATURE) == 0) {
        ui_set_temperature(v, valid);
    } else if (strcmp(topic, TOPIC_HUMIDITY) == 0) {
        ui_set_humidity(v, valid);
    }
}

static void mqtt_service()
{
    if (WiFi.status() != WL_CONNECTED) return;

    if (mqtt.connected()) {
        mqtt.loop();
        return;
    }
    static uint32_t last = 0;
    if (millis() - last < 3000) return;   /* non-blocking backoff */
    last = millis();

    /* LWT: broker publishes "offline" to TOPIC_STATUS if we drop */
    if (mqtt.connect(HOSTNAME, MQTT_USER, MQTT_PASS, TOPIC_STATUS, 0, true, "offline")) {
        mqtt.publish(TOPIC_STATUS, "online", true);
        mqtt.subscribe(TOPIC_TEMPERATURE);
        mqtt.subscribe(TOPIC_HUMIDITY);
        char m[96];
        snprintf(m, sizeof(m), "mqtt connected, ip=%s rssi=%d heap=%u",
                 WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                 (unsigned)ESP.getFreeHeap());
        logmsg(m);
    }
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

/* ---------------- Time ---------------- */
static void update_clock()
{
    struct tm ti;
    /* getLocalTime() reports false until NTP has set a plausible year. Fall
     * back to the raw system clock so the hands still sweep while Wi-Fi/NTP
     * come up -- otherwise the whole face looks dead. */
    bool synced = getLocalTime(&ti, 5);
    if (!synced) {
        time_t now = time(nullptr);
        localtime_r(&now, &ti);
    }

    /* Smooth second hand: interpolate between whole seconds using millis() */
    static int lastSec = -1;
    static uint32_t secMs = 0;
    if (ti.tm_sec != lastSec) { lastSec = ti.tm_sec; secMs = millis(); }
    float frac = (millis() - secMs) / 1000.0f;
    if (frac > 1.0f) frac = 1.0f;
    float secf = ti.tm_sec + frac;

    ui_set_time(ti.tm_hour, ti.tm_min, secf, ti.tm_wday, ti.tm_mday, ti.tm_mon);
    if (!synced) ui_set_time_unknown();   /* hands move; digital stays "--:--" */

    /* Night dimming */
    static int lastDim = -1;
    bool night = (DIM_START_HOUR < DIM_END_HOUR)
                 ? (ti.tm_hour >= DIM_START_HOUR && ti.tm_hour < DIM_END_HOUR)
                 : (ti.tm_hour >= DIM_START_HOUR || ti.tm_hour < DIM_END_HOUR);
    int want = night ? BL_NIGHT : BL_DAY;
    if (want != lastDim) { backlight_set((uint8_t)want); lastDim = want; }
}

/* ==================================================================== */
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
    ui_set_temperature(DUMMY_TEMP, true);
    ui_set_humidity(DUMMY_HUM, true);
#endif

    /* WiFi */
    WiFi.onEvent(wifi_event);
    wifi_start();
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        lv_timer_handler();   /* keep the face alive while connecting */
        delay(10);
    }

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
        wifi_start();   /* keep trying in the background */
    }

    /* NTP */
    configTzTime(TZ_INFO, "pool.ntp.org", "time.cloudflare.com");

    /* MQTT */
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(mqtt_cb);
    mqtt.setBufferSize(512);

    /* OTA + status page */
    server.on("/", handle_root);
    ElegantOTA.begin(&server);
    ElegantOTA.setAuth("admin", OTA_PASSWORD);
    server.begin();
}

void loop()
{
    lv_timer_handler();
    wifi_service();
    server.handleClient();
    ElegantOTA.loop();
    mqtt_service();

    static uint32_t last = 0;
    if (millis() - last >= 50) {   /* ~20 fps clock update for a smooth sweep */
        last = millis();
        update_clock();
    }
    delay(5);
}
