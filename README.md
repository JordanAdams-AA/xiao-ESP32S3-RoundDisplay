# XIAO ESP32-S3 Round Display — watch face

Analog watch face for the Seeed XIAO ESP32-S3 + Round Display (GC9A01 240x240),
recreating the reference layout: minute tick ring with numbers, white hour/minute
hands, green sweeping second hand, green digital time with weekday, and three
complications — temperature and humidity arc gauges (from Home Assistant over
MQTT) and a date circle. Time comes from NTP. Updates go over Wi-Fi (OTA).

This repo is **source only**. You build it once, then flash. See below.

## 1. Configure

```
cp include/secrets.h.example include/secrets.h
```

Edit `include/secrets.h` with your Wi-Fi, MQTT broker, and an OTA password.
Check `include/config.h` — the MQTT topics are already set to your
`temphumzolder` sensor via `mqtt_statestream`. Timezone is Europe/Brussels.

Tip for the very first flash: set `USE_DUMMY_DATA` to `1` in `config.h` to see
the gauges filled before MQTT is wired up. Set it back to `0` afterwards.

## 2. Build

You need [PlatformIO](https://platformio.org/) (`pip install platformio`, or the
VS Code extension). First build downloads the ESP32 toolchain + libraries
(~1-2 GB) and needs internet access.

```
pio run            # compiles
./build.sh         # compiles AND produces dist/firmware.bin + dist/merged.bin
```

## 3. First flash (once, over USB)

The first flash needs the board plugged into a PC. Two ways:

**Easiest**, on a PC with PlatformIO and the board on USB:
```
pio run -t upload
```

**Or** flash the full image from any PC (no PlatformIO needed):
```
esptool.py --chip esp32s3 write_flash 0x0 dist/merged.bin
```
or drag `dist/merged.bin` into <https://espressif.github.io/esp-launchpad/> in
Chrome and flash at offset `0x0`.

If the XIAO isn't detected: hold the **BOOT** button while plugging in USB, then
release (bootloader mode).

## 4. Later updates (over Wi-Fi, no USB)

After the first flash the device serves a page at `http://<device-ip>/` with a
link to `/update`. Build a new `firmware.bin`, open `/update`, log in
(user `admin`, password = your `OTA_PASSWORD`), and upload it. The build
environment never needs to touch the board.

## 5. MQTT / Home Assistant

Topics the firmware listens on (from `config.h`):
```
homeassistant/sensor/temphumzolder_temperature/state
homeassistant/sensor/temphumzolder_humidity/state
```
See `extras/home-assistant.md` for the statestream config you're using and a
retained-value alternative. Device logs are on `xiao/log`, availability on
`xiao/status`:
```
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'xiao/#' -v
```

## Troubleshooting

- **Colours look wrong / red and blue swapped / tinted:** flip
  `LV_COLOR_16_SWAP` between `0` and `1` in `include/lv_conf.h` and rebuild. The
  flush callback switches its draw call to match automatically. This is the most
  likely first-flash issue and the one thing that couldn't be tested here.
- **Screen stays black:** check the display pins in `config.h` against the Seeed
  wiki; confirm `gfx->begin()` runs; check the backlight pin (`PIN_TFT_BL`).
- **Backlight/dimming does nothing:** the backlight pin is probably different on
  your board revision — correct `PIN_TFT_BL` from the wiki.
- **Time stays `--:--`:** no NTP yet. Check Wi-Fi and that UDP/123 isn't blocked.
- **Gauges stay `--`:** no MQTT value yet. Verify with `mosquitto_sub`, and note
  statestream only publishes on change (no retain) — see extras.

## What's implemented vs. needs a hardware check

Implemented: display + LVGL, full watch face, NTP time with smooth second hand,
weekday + date, Wi-Fi, MQTT subscribe with validation, MQTT logging + LWT,
ElegantOTA web update, night dimming.

**Needs verification on real hardware** (couldn't be tested without the board):
exact pin mapping, `LV_COLOR_16_SWAP` value, backlight PWM pin, and the precise
on-screen positions/sizes vs. the reference photo.

Simplifications vs. the reference image: minute numbers are upright (not rotated
to follow the circle), and the gauges use `TEMP`/`HUM` text captions instead of
icon glyphs. Both are noted in `AGENTS.md` as easy follow-ups.
