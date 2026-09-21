# Agent notes for this project

Read this before changing anything. Keep it updated.

## Board & hardware
- Seeed **XIAO ESP32-S3** (8 MB flash, 8 MB octal PSRAM). Wi-Fi 2.4 GHz.
- **Seeed Round Display for XIAO**: GC9A01 240x240 IPS, CST816S touch (I2C),
  PCF8563 RTC (I2C), microSD slot.
- Confirmed pin mapping is in `include/config.h`. **Verify against the Seeed
  wiki** ("Getting Started with Seeed Studio Round Display for XIAO") before
  trusting it. Do not guess pins.

## Build / flash / debug
- Build:        `pio run`
- Full image:   `./build.sh`  → produces `dist/firmware.bin` and `dist/merged.bin`
- Flash (USB):  `pio run -t upload`   (only where the board is plugged in)
- Flash (OTA):  upload `dist/firmware.bin` at `http://<ip>/update` (user `admin`,
                password = `OTA_PASSWORD` from secrets.h), or
                `pio run -t upload --upload-port <ip>` if the machine can reach it.
- Serial monitor is usually unavailable (remote build). Debug via MQTT instead:
  `mosquitto_sub -h <broker> -u <user> -P <pass> -t 'xiao/#' -v`
- **Always compile after every change and fix errors before moving on.**

## Layout
- `include/config.h`   pins, MQTT topics, ranges, timezone, dimming, dummy flag
- `include/lv_conf.h`  LVGL config (colour swap toggle lives here)
- `include/secrets.h`  Wi-Fi/MQTT/OTA creds (gitignored; copy from .example)
- `src/ui.cpp`         all LVGL widgets (face, hands, arcs, date)
- `src/main.cpp`       display driver, Wi-Fi, NTP, MQTT, OTA, main loop

## Known gotchas
- If colours are wrong, flip `LV_COLOR_16_SWAP` in `include/lv_conf.h` (the flush
  callback in main.cpp switches its draw call to match automatically).
- Changing the partition table later needs another USB flash, so it is fixed to
  `default_8MB.csv` from the start.
- `board_build.arduino.memory_type = qio_opi` enables the octal PSRAM. Leave it.

## Not yet implemented (good next tasks)
- Touch (CST816S) interaction / multiple faces.
- PCF8563 RTC as an offline time fallback.
- MQTT-triggered pull-OTA (download firmware.bin from a URL on `xiao/cmd/update`).
- LVGL PC/SDL simulator target for a visual diff against the reference image.
- Rotated (tangential) minute numbers and icon glyphs instead of TEMP/HUM captions.
