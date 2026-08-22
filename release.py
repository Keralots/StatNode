#!/usr/bin/env python3
"""
End-to-end release builder for the StatNode web flasher.

Runs the whole release pipeline for the browser flasher at docs/:
    1. Reads FW_VERSION from include/config.h  ->  v<ver>
    2. Locates the PlatformIO CLI (PATH, then the standard penv install)
    3. Builds every board env, one PlatformIO invocation per env
       (sequential - two parallel builds race on .sconsign)
    4. Merges bootloader + partitions + app into a single "Full" image per
       env (flashed at 0x0, what ESP Web Tools writes)
    5. Copies the Full.bin images into docs/firmware/latest/ as
       StatNode-<env>-v<ver>-Full.bin and writes the VERSION file the page reads
    6. Writes the GitHub Release images into release/v<ver>/ with the same
       names create_firmware.py uses, so existing releases and the README stay
       consistent:
         firmware-v<ver>-<env>.bin           (new device, full 0x0 image)
         OTA_ONLY_firmware-v<ver>-<env>.bin  (existing device, portal update)

The web flasher reads the firmware id (== env name) and chip family from the
BOARDS map in docs/flasher.js. Every board has its own image; none are shared.

This is the superset of create_firmware.py: it does everything that script does
for release/ AND also populates docs/firmware/latest/ for the web flasher.
create_firmware.py is kept for the release/-only workflow.

Usage:
    python release.py                 # build + package every env
    python release.py --skip-build    # package whatever .pio/build already has
    python release.py v1.3.0          # override the version string
    python release.py v1.3.0 esp32c3  # single env (repeat flag not supported;
                                      #   omit to do all)
"""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

# PlatformIO envs published by the web flasher. The env name is also the
# `firmware` id in docs/flasher.js and the token in every output filename.
# Keep in sync with ENVS in create_firmware.py and the BOARDS map in flasher.js.
ENVS = [
    "esp32s3",       # LOLIN ESP32-S3 Mini + ST7789 240x240 (default)
    "esp32s3_zero",  # Waveshare ESP32-S3-Zero + ST7789 240x240
    "ws_lcd_154",    # Waveshare ESP32-S3-Touch-LCD-1.54 240x240
    "esp32c3",       # LOLIN ESP32-C3 Mini + ST7789 240x240
    "jc3248w535",    # Guition JC3248W535 3.5" 320x480 (16MB flash)
]

# Flash offsets for the ESP32-S3 and ESP32-C3 (bootloader starts at 0x0).
BOOTLOADER_OFFSET = 0x0
PARTITIONS_OFFSET = 0x8000
FIRMWARE_OFFSET = 0x10000

REPO_ROOT = Path(__file__).resolve().parent
CONFIG_H = REPO_ROOT / "include" / "config.h"
DOCS_LATEST = REPO_ROOT / "docs" / "firmware" / "latest"


def read_version() -> str:
    """Extract FW_VERSION from include/config.h, normalised to v<ver>."""
    if not CONFIG_H.exists():
        sys.exit(f"error: {CONFIG_H} not found")
    pat = re.compile(r'#define\s+FW_VERSION\s+"([^"]+)"')
    for line in CONFIG_H.read_text(encoding="utf-8").splitlines():
        m = pat.search(line)
        if m:
            ver = m.group(1).strip()
            return ver if ver.startswith("v") else "v" + ver
    sys.exit("error: FW_VERSION not found in include/config.h")


def locate_pio() -> str:
    """Locate the PlatformIO CLI executable."""
    for name in ("pio", "pio.exe", "platformio", "platformio.exe"):
        found = shutil.which(name)
        if found:
            return found
    candidates = [
        Path.home() / ".platformio" / "penv" / "Scripts" / "pio.exe",
        Path.home() / ".platformio" / "penv" / "Scripts" / "platformio.exe",
        Path.home() / ".platformio" / "penv" / "bin" / "pio",
        Path.home() / ".platformio" / "penv" / "bin" / "platformio",
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    sys.exit(
        "error: pio executable not found.\n"
        "  Tried PATH and the standard ~/.platformio/penv locations.\n"
        "  Install PlatformIO Core or add it to PATH."
    )


def run(cmd, cwd=REPO_ROOT):
    """Run a subprocess, exit on failure."""
    print(f"\n$ {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd, cwd=cwd)
    if result.returncode != 0:
        sys.exit(f"error: command failed with exit code {result.returncode}")


def build_env(pio_path: str, env: str):
    """Build a single env. One invocation per env - parallel builds race the
    shared .sconsign, so never batch them."""
    run([pio_path, "run", "-e", env])


def build_dir(env: str) -> Path:
    return REPO_ROOT / ".pio" / "build" / env


def merge_full_bin(env: str, out_path: Path):
    """Merge bootloader + partitions + firmware into a single 0x0 image.

    The 0xFF fill between partitions (0x8000) and the app (0x10000) also leaves
    the otadata region blank, so the bootloader starts from app0.
    """
    bd = build_dir(env)
    bootloader = bd / "bootloader.bin"
    partitions = bd / "partitions.bin"
    firmware = bd / "firmware.bin"
    for p in (bootloader, partitions, firmware):
        if not p.exists():
            sys.exit(f"error: {p} not found - run a build first (omit --skip-build).")

    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "wb") as out:
        bl = bootloader.read_bytes()
        out.write(bl)
        out.write(b"\xFF" * (PARTITIONS_OFFSET - len(bl)))

        pt = partitions.read_bytes()
        out.write(pt)
        out.write(b"\xFF" * (FIRMWARE_OFFSET - (PARTITIONS_OFFSET + len(pt))))

        out.write(firmware.read_bytes())

    size = out_path.stat().st_size
    print(f"  Full: {out_path.relative_to(REPO_ROOT)} ({size / 1024:.1f} KB)")


def copy_ota_bin(env: str, out_path: Path):
    """Copy firmware.bin verbatim as the OTA update image."""
    firmware = build_dir(env) / "firmware.bin"
    if not firmware.exists():
        sys.exit(f"error: {firmware} not found - run a build first (omit --skip-build).")
    out_path.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(firmware, out_path)
    size = out_path.stat().st_size
    print(f"  OTA:  {out_path.relative_to(REPO_ROOT)} ({size / 1024:.1f} KB)")


def write_version_file(version: str):
    DOCS_LATEST.mkdir(parents=True, exist_ok=True)
    (DOCS_LATEST / "VERSION").write_text(version + "\n", encoding="utf-8")


def find_old_full_bins(version: str):
    """List Full.bin files in docs/firmware/latest/ not for this version."""
    if not DOCS_LATEST.exists():
        return []
    pat = re.compile(r"^StatNode-(.+)-(v[^-]+)-Full\.bin$")
    old = []
    for f in DOCS_LATEST.iterdir():
        m = pat.match(f.name)
        if m and m.group(2) != version:
            old.append(f.name)
    return sorted(old)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("version", nargs="?", default=None,
                        help="Version override (default: read from include/config.h)")
    parser.add_argument("env", nargs="?", default=None, choices=ENVS,
                        help="Single env to build (default: all of them)")
    parser.add_argument("--skip-build", action="store_true",
                        help="Skip the PlatformIO build (assume .pio/build is current)")
    args = parser.parse_args()

    version = args.version or read_version()
    if not version.startswith("v"):
        version = "v" + version

    envs = [args.env] if args.env else ENVS
    ota_dir = REPO_ROOT / "release" / version

    print(f"StatNode web-flasher release: {version}")
    print("Envs: " + ", ".join(envs))

    if not args.skip_build:
        pio = locate_pio()
        print(f"PlatformIO: {pio}")
        for env in envs:
            build_env(pio, env)
    else:
        print("Skipping build (--skip-build)")

    print("\n--- Web flasher images (docs/firmware/latest/) ---")
    for env in envs:
        out = DOCS_LATEST / f"StatNode-{env}-{version}-Full.bin"
        merge_full_bin(env, out)
    write_version_file(version)
    print(f"  VERSION  ({version})")

    print(f"\n--- GitHub Release images (release/{version}/) ---")
    for env in envs:
        # Full 0x0 image for new devices - same bytes as the docs flasher image,
        # named by env to match create_firmware.py and the README.
        full_src = DOCS_LATEST / f"StatNode-{env}-{version}-Full.bin"
        full_out = ota_dir / f"firmware-{version}-{env}.bin"
        full_out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(full_src, full_out)
        print(f"  Full: {full_out.relative_to(REPO_ROOT)} ({full_out.stat().st_size / 1024:.1f} KB)")
        # OTA-only image for existing devices (portal Maintenance page).
        ota_out = ota_dir / f"OTA_ONLY_firmware-{version}-{env}.bin"
        copy_ota_bin(env, ota_out)

    print("\n" + "=" * 60)
    print(f"Release {version} ready.")
    print("=" * 60)

    old = find_old_full_bins(version)
    if old:
        print("\nOlder Full.bin files still in docs/firmware/latest/ "
              "(remove with `git rm` when no longer needed):")
        for name in old:
            print(f"  {name}")

    print("\nNext steps:")
    print("  git add docs/firmware/latest/ include/config.h")
    print(f'  git commit -m "release {version}"')
    print(f"  gh release create {version} release/{version}/*.bin --notes \"...\"")
    print("  git push")


if __name__ == "__main__":
    main()
