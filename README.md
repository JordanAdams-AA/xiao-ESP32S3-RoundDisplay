# XIAO ESP32-S3 Round Display — watch face

Analog watch face for the Seeed XIAO ESP32-S3 + Round Display (GC9A01 240x240).
Minute tick ring with numbers, white hour/minute hands, a green second hand that
ticks once per second, digital time with weekday and date, and temperature and
humidity from Home Assistant over MQTT.

Time comes from the board's RTC, corrected by NTP. Firmware updates go over
Wi-Fi. Swipe between pages; rotation, clock and time zone are set on the device.

This repo is **source only**. You build it once, then flash.

---

## 1. Hardware

- **Seeed XIAO ESP32-S3** (8 MB flash, 8 MB octal PSRAM)
- **Seeed Round Display for XIAO** — GC9A01 240x240 IPS, CHSC6X capacitive
  touch, PCF8563 RTC

Two things on the display board are easy to miss and both cost real debugging
time:

- **Fit the U.FL antenna.** The XIAO ESP32-S3 has no usable on-board antenna.
  Without it the radio associates but sits around −85 dBm, which shows up later
  as MQTT timeouts rather than as an obvious Wi-Fi failure. With it, expect
  −40 to −60 dBm in the same room as the AP. The firmware logs RSSI on every
  association so you can check.
- **Fit a backup cell for the RTC** if you want the time to survive a power
  cut. Without one the PCF8563 raises its voltage-low flag, the firmware
  refuses to trust it, and the clock waits for NTP instead.

---

## 2. Configure

```
cp include/secrets.h.example include/secrets.h
```

Fill in Wi-Fi, MQTT broker and an OTA password. `secrets.h` is gitignored.

Check `include/config.h` for the MQTT topics, pin mapping and the tunables
listed under [Settings and tunables](#7-settings-and-tunables).

> **Watch the spelling of your MQTT credentials.** A transposed username
> (`xaio` vs `xiao`) reaches the broker and is rejected with a plain
> "not authorized", which looks identical to a wrong password. Test the same
> user/password pair from a PC before blaming the firmware:
> `mosquitto_sub -h <broker> -u <user> -P <pass> -t 'homeassistant/#' -v`

Tip for the very first flash: set `USE_DUMMY_DATA` to `1` in `config.h` to see
the read-outs filled before MQTT is wired up. It also bypasses the availability
checks, so the values stay on screen with no broker at all.

---

## 3. Build

You need [PlatformIO](https://platformio.org/). `pip install platformio`, or the
VS Code extension.

```
pio run            # compiles
./build.sh         # compiles AND produces dist/firmware.bin + dist/merged.bin
```

If you installed PlatformIO into a virtualenv, `pio` will not be on your PATH.
Either activate the venv first, or call it by path:

```
.venv/Scripts/pio.exe run -e seeed_xiao_esp32s3        # Windows
.venv/bin/pio run -e seeed_xiao_esp32s3                # Linux/macOS
```

The first build downloads the ESP32 toolchain and libraries (1-2 GB) and needs
internet access.

**`GFX Library for Arduino` is pinned to 1.4.9 on purpose.** 1.6.x requires
Arduino core 3.x, while the `espressif32` platform installs core 2.0.17; with a
`^` range the resolver picks 1.6.x and the build dies on a missing
`esp32-hal-periman.h`. Leave it pinned.

---

## 4. First flash (once, over USB)

```
pio run -t upload
```

Or flash the full image from any PC with no PlatformIO:

```
esptool.py --chip esp32s3 write_flash 0x0 dist/merged.bin
```

or drag `dist/merged.bin` into <https://espressif.github.io/esp-launchpad/> in
Chrome and flash at offset `0x0`.

If the XIAO is not detected, hold **BOOT** while plugging in USB, then release.

**Serial output needs USB CDC**, which `platformio.ini` enables with
`ARDUINO_USB_MODE=1` and `ARDUINO_USB_CDC_ON_BOOT=1`. Without those, `Serial`
goes to the UART0 pins and every log line is invisible over USB.

```
pio device monitor -b 115200
```

---

## 5. Later updates (over Wi-Fi, no USB)

After the first flash the device serves a status page at `http://<device-ip>/`
showing local time, the UTC offset in use, RSSI, free heap and uptime, with a
link to `/update`. Build a new `firmware.bin`, open `/update`, log in
(user `admin`, password = your `OTA_PASSWORD`) and upload it.

---

## 6. Using it

### Pages

Pages sit on a grid. Swipe to move between them; there is no timed rotation.

```
        watch  <-->  climate
          |
        display     (rotate the UI)
          |
        set time    (hours / minutes)
          |
        time zone   (UTC offset)
```

- **watch** — analog face, digital time, weekday, date ring, plus a flame with
  the temperature and a droplet with the humidity either side of the hub.
- **climate** — the same two read-outs stacked around a large clock and the
  written-out date, with units.
- **display** — `ROTATE` steps the whole UI 90° per press so the face can be
  oriented to suit however the USB-C connector sits. Saved in NVS.
- **set time** — arrows above and below the clock change hours and minutes.
  They move the clock itself, so there is nothing to confirm. Writes the RTC.
- **time zone** — UTC offset in whole hours.

### Time

The **RTC is the clock of record**. It is read at boot, so the watch knows the
time with no network at all, and the system clock is realigned to it every
10 minutes. NTP does not set the clock directly: each successful sync is
written *back* to the RTC, which is what corrects drift or a freshly-batteried
chip. The RTC always holds UTC; local time is applied through the offset, so
changing time zone never rewrites the chip.

> **The time zone is a fixed offset, not a DST rule.** The clock will not jump
> on the last Sunday of March or October. If your region uses summer time you
> adjust the offset by hand twice a year — for example Brussels is `UTC+1` in
> winter and `UTC+2` in summer. This is deliberate: a hardcoded EU rule would
> be wrong for most of the world.

### When readings are unavailable

Temperature and humidity are shown only while the MQTT session is up and the
value is recent. Otherwise the icon, the number and the unit are hidden
outright rather than falling back to `--`, so an empty space means "not
available" and a number on screen is always a live reading. The climate page
shows "no sensor data" when both are hidden.

---

## 7. Settings and tunables

All in `include/config.h`:

| Setting | Purpose |
| --- | --- |
| `DEFAULT_UTC_OFFSET_HOURS` | Offset before anything is saved on the device |
| `CLIMATE_STALE_SECONDS` | Hide readings older than this; `0` relies on the MQTT link alone |
| `NTP_SYNC_INTERVAL_SECONDS` | How often the network clock is re-checked |
| `RTC_RESYNC_SECONDS` | How often the system clock is realigned to the RTC |
| `SWIPE_INVERT` | Set to `1` if swipes move the pages the wrong way |
| `SWIPE_MIN_PX` | Travel before a drag counts as a swipe |
| `GFX_SPI_HZ` | Display clock; drop to `40000000` if the screen tears |
| `LVBUF_LINES` | Height of the LVGL render buffer |
| `PERF_LOG_SECONDS` | Render timing to serial; `0` disables |
| `USE_DUMMY_DATA` | Fixed values with no broker, for bench testing |
| `DIM_START_HOUR` / `DIM_END_HOUR` / `BL_NIGHT` | Night dimming |
| `TEMP_MIN`/`TEMP_MAX`, `HUM_MIN`/`HUM_MAX` | Gauge ranges |

---

## 8. MQTT / Home Assistant

Topics the firmware listens on (from `config.h`):

```
homeassistant/sensor/temphumzolder_temperature/state
homeassistant/sensor/temphumzolder_humidity/state
```

See `extras/home-assistant.md` for the statestream config and a retained-value
alternative. Device logs are published on `xiao/log`, availability on
`xiao/status` (retained, with a last-will of `offline`):

```
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'xiao/#' -v
```

**Retained values are worth setting up.** `mqtt_statestream` publishes only on
change and does not retain, so after a reconnect the device has nothing to show
until the sensor next moves — which can be a long time for a stable room.

A non-numeric payload (Home Assistant publishes `unavailable` when a sensor
drops) is treated as no reading and hides that row.

---

## 9. How it is put together

**Two pinned FreeRTOS tasks.** `ui_task` on core 1 renders and never blocks;
`net_task` on core 0 runs Wi-Fi, MQTT and the web server next to the Wi-Fi
driver, where it is free to block. Before the split, `mqtt.connect()` to an
unreachable broker froze the second hand for seconds at a time.

Neither LVGL nor PubSubClient is thread-safe, so the boundary is strict. The
MQTT callback parks values in a mutex-guarded struct that `ui_task` drains
rather than drawing from the network task, and every MQTT call takes a
recursive mutex — recursive because `mqtt_service()` logs while holding it, and
Wi-Fi events log from a third task. `ui_task` also owns the I2C bus, so all
touch and RTC traffic runs there and needs no second lock.

**The face redraws once per second**, on the second boundary. Rendering is
cheap (typically 0 ms, with a ~50 ms spike on the repaint), so the UI has
plenty of headroom.

**Touch** is a CHSC6X at `0x2E`. It has no gesture register, so swipes are
reconstructed from each touch's start and end coordinates, and it only
acknowledges I2C while a finger is down — it cannot be found by a bus scan.
Its interrupt line on D7 is latched in an ISR; polling the pin missed most
swipes. Raw coordinates are mapped into rotated screen space inside the driver,
so taps and swipe directions both follow whatever rotation is selected.

**The flame and droplet are drawn, not glyphs.** LVGL ships a droplet but no
flame. `lv_canvas_draw_polygon()` hangs outright on a concave outline — its
software renderer assumes convex — so the icons are rasterised with a
supersampled scanline fill written straight into the canvas buffer. The flame
is recoloured to follow the temperature.

---

## 10. Troubleshooting

- **Colours look wrong / red and blue swapped:** flip `LV_COLOR_16_SWAP`
  between `0` and `1` in `include/lv_conf.h` and rebuild. The flush callback
  switches its draw call to match.
- **Screen stays black:** check the display pins in `config.h` against the
  Seeed wiki, and the backlight pin `PIN_TFT_BL`.
- **Nothing on serial:** see the USB CDC note in section 4.
- **Wi-Fi associates then drops, or MQTT times out intermittently:** check
  RSSI in the boot log or on the status page. Below about −80 dBm, fit or
  re-seat the U.FL antenna.
- **Wi-Fi picks a distant access point:** with several APs sharing one SSID the
  default fast scan takes the first match, not the strongest. The firmware
  scans all channels and sorts by signal; the boot log prints the chosen
  channel and RSSI.
- **MQTT rejected with "not authorized":** the broker is reachable and refusing
  the credentials. Test the same pair from a PC — see section 2.
- **Swipes go the wrong way:** set `SWIPE_INVERT` to `1`.
- **Swipes are ignored:** every detected gesture logs
  `touch: gesture=N travel=N page=N`. No line at all means the panel is not
  reporting; a line with small `travel` means the swipe was below
  `SWIPE_MIN_PX`.
- **Clock is a whole number of hours out:** the time zone offset, not the
  clock. See the DST note in section 6. The status page shows both local time
  and the offset in use.
- **Readings vanish while MQTT is connected:** the value aged past
  `CLIMATE_STALE_SECONDS`. A sensor that publishes only on change can be
  legitimately quiet for a long time; raise it, or set it to `0`.
