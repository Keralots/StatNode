# Firmware binaries for the web flasher

These files are produced automatically by `release.py` at the repo root
(`python release.py`), which builds every board env, merges each into a "Full"
image, and drops them here with the matching `VERSION`. You normally don't add
them by hand - the manual steps below are just for reference.

The flasher reads the version string from `VERSION` (a single line, e.g. `v1.3.0`)
and expects one full-image binary per board alongside it:

    firmware/latest/
      VERSION                              ← one line, e.g. v1.3.0
      StatNode-esp32s3-v1.3.0-Full.bin     ← LOLIN ESP32-S3 Mini
      StatNode-esp32s3_zero-v1.3.0-Full.bin
      StatNode-ws_lcd_154-v1.3.0-Full.bin
      StatNode-esp32c3-v1.3.0-Full.bin
      StatNode-jc3248w535-v1.3.0-Full.bin  ← Guition JC3248W535 (16MB)

Notes
-----
- The filename pattern is `StatNode-<env>-<VERSION>-Full.bin`, where `<env>` is
  the PlatformIO environment and the `firmware` value in the `BOARDS` map in
  `flasher.js`.
- Every board has its own image (unlike SmallOLED, none are shared).
- "Full" means a merged image (bootloader + partitions + app) flashed at offset
  0x0. ESP Web Tools writes it to 0x0 - exactly what the README's manual `0x0`
  step does.
- The chip family per board is set in `flasher.js` (esp32c3 = ESP32-C3, the rest
  = ESP32-S3). ESP Web Tools uses it to verify the connected chip.
- To publish a new release, bump `VERSION` and add the matching `-Full.bin`
  files. No code change needed.

How to build a merged image by hand (from the project, esptool):

    esptool.py --chip esp32s3 merge_bin -o StatNode-esp32s3-v1.3.0-Full.bin \
      0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin

Use `--chip esp32c3` for the `esp32c3` env.
