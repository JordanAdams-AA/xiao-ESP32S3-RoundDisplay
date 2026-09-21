#!/usr/bin/env bash
# Build the firmware and produce two files in dist/:
#   firmware.bin  -> for OTA updates
#   merged.bin    -> full flash image for the FIRST flash (write at offset 0x0)
#
# Requirements: PlatformIO (pip install platformio) and esptool
# (PlatformIO already bundles esptool; this script uses the bundled one).
set -e

ENV=seeed_xiao_esp32s3
BUILD=".pio/build/${ENV}"
OUT="dist"

echo ">> Building..."
pio run -e "${ENV}"

mkdir -p "${OUT}"
cp "${BUILD}/firmware.bin" "${OUT}/firmware.bin"

# Locate boot_app0.bin inside the installed framework package.
BOOT_APP0=$(find "${HOME}/.platformio/packages" -name boot_app0.bin 2>/dev/null | head -n1)
if [ -z "${BOOT_APP0}" ]; then
  echo "!! boot_app0.bin not found; merged.bin skipped. firmware.bin (OTA) is ready."
  echo "   For a first flash without merged.bin, use: pio run -t upload  (needs USB)."
  exit 0
fi

echo ">> Merging full image..."
# Use PlatformIO's bundled esptool so versions always match the build.
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 merge_bin \
  -o "${OUT}/merged.bin" \
  --flash_mode dio --flash_freq 80m --flash_size 8MB \
  0x0     "${BUILD}/bootloader.bin" \
  0x8000  "${BUILD}/partitions.bin" \
  0xe000  "${BOOT_APP0}" \
  0x10000 "${BUILD}/firmware.bin"

echo ""
echo ">> Done."
ls -lh "${OUT}"/*.bin
echo ""
echo "First flash (any PC, no PlatformIO needed): write dist/merged.bin at 0x0"
echo "  esptool.py --chip esp32s3 write_flash 0x0 dist/merged.bin"
echo "  or drag it into https://espressif.github.io/esp-launchpad/ (Chrome)."
