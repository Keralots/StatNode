#!/usr/bin/env python3
"""
Create firmware binaries for both OTA update and full web flasher installation.

The target board is selected at BUILD TIME via a dedicated PlatformIO
environment, so every maintained 240x240 board gets its own pair of binaries.
This script builds the requested environment(s) and packages them.

Usage:
    python create_firmware.py                     # FW_VERSION from config.h, ALL envs
    python create_firmware.py v1.0.0              # Version v1.0.0, ALL envs
    python create_firmware.py v1.0.0 esp32c3      # Version v1.0.0, esp32c3 only
    python create_firmware.py v1.0.0 esp32c3 --no-build   # Package an already-built env

Output files (in release/{version}/ folder):
    release/v1.0.0/firmware-v1.0.0-esp32c3.bin           (full flash @ 0x0)
    release/v1.0.0/OTA_ONLY_firmware-v1.0.0-esp32c3.bin  (OTA update)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

RELEASES_DIR = 'release'

# Flash offsets (ESP32-S3 and ESP32-C3 both boot from 0x0)
BOOTLOADER_OFFSET = 0x0
PARTITIONS_OFFSET = 0x8000
FIRMWARE_OFFSET = 0x10000

# PlatformIO environments to package. Each env has its own build directory
# under .pio/build/, so variants never share object files and don't need a
# clean between builds.
ENVS = [
    'esp32s3',       # LOLIN ESP32-S3 Mini + ST7789
    'esp32s3_zero',  # Waveshare ESP32-S3-Zero + ST7789
    'ws_lcd_154',    # Waveshare ESP32-S3-Touch-LCD-1.54
    'esp32c3',       # LOLIN ESP32-C3 Mini + ST7789
]


def default_version():
    """Read FW_VERSION from include/config.h and prefix it with 'v'."""
    config = os.path.join('include', 'config.h')
    try:
        with open(config, encoding='utf-8') as f:
            match = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
        if match:
            return 'v' + match.group(1)
    except OSError:
        pass
    return None


def find_platformio():
    """Locate the platformio executable (PATH first, then the standard penv)."""
    for name in ('platformio', 'pio'):
        found = shutil.which(name)
        if found:
            return found

    # Fall back to the default install location used by the PlatformIO IDE/CLI
    candidates = [
        os.path.expanduser('~/.platformio/penv/Scripts/platformio.exe'),  # Windows
        os.path.expanduser('~/.platformio/penv/bin/platformio'),          # Linux/macOS
    ]
    for candidate in candidates:
        if os.path.exists(candidate):
            return candidate

    return None


def build_dir_for(env):
    """Return the PlatformIO build output directory for an environment."""
    return os.path.join('.pio', 'build', env)


def binary_paths(env):
    """Return (bootloader, partitions, firmware) paths for an environment."""
    build_dir = build_dir_for(env)
    return (
        os.path.join(build_dir, 'bootloader.bin'),
        os.path.join(build_dir, 'partitions.bin'),
        os.path.join(build_dir, 'firmware.bin'),
    )


def build_variant(env):
    """Compile the firmware for the given PlatformIO environment."""
    pio = find_platformio()
    if not pio:
        print("Error: could not find the 'platformio' (or 'pio') executable.")
        print("Add it to PATH, or build manually with: platformio run -e " + env)
        return False

    print(f"\nBuilding firmware (env: {env}) ...")
    print(f"  Using: {pio}")
    result = subprocess.run([pio, 'run', '-e', env])
    if result.returncode != 0:
        print(f"Error: build failed for env '{env}' (exit code {result.returncode}).")
        return False
    return True


def get_output_filenames(version, env):
    """Generate output filenames based on version and environment."""
    base_name = f"{version}-{env}"

    # Create version-specific folder under release/
    output_dir = os.path.join(RELEASES_DIR, version)

    merged_name = f"firmware-{base_name}.bin"
    ota_name = f"OTA_ONLY_firmware-{base_name}.bin"

    return (
        output_dir,
        os.path.join(output_dir, merged_name),
        os.path.join(output_dir, ota_name)
    )


def create_ota_binary(firmware_path, output_path):
    """Create OTA-only firmware binary (just firmware.bin copy)."""

    if not os.path.exists(firmware_path):
        print(f"Error: {firmware_path} not found!")
        print("Please build the firmware first (omit --no-build).")
        return False

    firmware_size = os.path.getsize(firmware_path)

    print(f"\nCreating OTA firmware: {output_path}")
    print(f"  Source: {firmware_path}")
    print(f"  Size: {firmware_size} bytes ({firmware_size / 1024:.1f} KB)")

    with open(firmware_path, 'rb') as infile:
        with open(output_path, 'wb') as outfile:
            outfile.write(infile.read())

    return True


def create_merged_binary(bootloader_path, partitions_path, firmware_path, output_path):
    """Merge bootloader, partitions, and firmware into single binary."""

    # Check if all input files exist
    for filepath in [bootloader_path, partitions_path, firmware_path]:
        if not os.path.exists(filepath):
            print(f"Error: {filepath} not found!")
            print("Please build the firmware first (omit --no-build).")
            return False

    print(f"\nCreating merged firmware: {output_path}")
    print(f"  Bootloader: {bootloader_path} @ 0x{BOOTLOADER_OFFSET:X}")
    print(f"  Partitions: {partitions_path} @ 0x{PARTITIONS_OFFSET:X}")
    print(f"  Firmware:   {firmware_path} @ 0x{FIRMWARE_OFFSET:X}")

    with open(output_path, 'wb') as outfile:
        # Write bootloader at 0x0
        with open(bootloader_path, 'rb') as f:
            bootloader_data = f.read()
            outfile.write(bootloader_data)
            bootloader_size = len(bootloader_data)

        # Pad to partitions offset (0x8000). The 0xFF fill also leaves the
        # otadata region blank, so the bootloader starts from app0.
        padding_size = PARTITIONS_OFFSET - bootloader_size
        outfile.write(b'\xFF' * padding_size)

        # Write partitions at 0x8000
        with open(partitions_path, 'rb') as f:
            partitions_data = f.read()
            outfile.write(partitions_data)
            partitions_size = len(partitions_data)

        # Pad to firmware offset (0x10000)
        current_pos = PARTITIONS_OFFSET + partitions_size
        padding_size = FIRMWARE_OFFSET - current_pos
        outfile.write(b'\xFF' * padding_size)

        # Write firmware at 0x10000
        with open(firmware_path, 'rb') as f:
            firmware_data = f.read()
            outfile.write(firmware_data)

    total_size = os.path.getsize(output_path)
    print(f"  Total size: {total_size} bytes ({total_size / 1024:.1f} KB)")

    return True


def process_variant(version, env, no_build):
    """Build (unless --no-build) and package a single environment."""
    print("\n" + "=" * 60)
    print(f"Environment: {env}")
    print("=" * 60)

    if not no_build:
        if not build_variant(env):
            return False

    bootloader_path, partitions_path, firmware_path = binary_paths(env)
    _, merged_path, ota_path = get_output_filenames(version, env)

    success_merged = create_merged_binary(
        bootloader_path, partitions_path, firmware_path, merged_path)
    success_ota = create_ota_binary(firmware_path, ota_path)

    if success_merged and success_ota:
        print(f"\n  Web Flasher (full):  {merged_path}")
        print(f"  OTA Update:          {ota_path}")
        return True

    return False


def main():
    parser = argparse.ArgumentParser(
        description='Create firmware binaries for OTA and web flasher',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
Examples:
    python create_firmware.py                   # FW_VERSION from config.h, all envs
    python create_firmware.py v1.0.0            # All envs
    python create_firmware.py v1.0.0 esp32c3    # esp32c3 only
        '''
    )
    parser.add_argument(
        'version',
        nargs='?',
        default=None,
        help='Firmware version (e.g., v1.0.0). Omit to use FW_VERSION from include/config.h.'
    )
    parser.add_argument(
        'env',
        nargs='?',
        choices=ENVS,
        default=None,
        help='PlatformIO environment. Omit to build ALL of: ' + ', '.join(ENVS)
    )
    parser.add_argument(
        '--no-build',
        action='store_true',
        help='Skip compiling; package binaries already built in .pio/build/.'
    )

    args = parser.parse_args()

    version = args.version or default_version()
    if not version:
        print("Error: could not read FW_VERSION from include/config.h; "
              "pass a version explicitly.")
        return 1

    # Determine which environment(s) to process
    envs = [args.env] if args.env else ENVS

    # Create release directory if needed
    output_dir = os.path.join(RELEASES_DIR, version)
    os.makedirs(output_dir, exist_ok=True)

    print("=" * 60)
    print(f"Creating firmware binaries")
    print(f"  Version: {version}")
    print(f"  Environments: {', '.join(envs)}")
    print(f"  Build: {'skipped (--no-build)' if args.no_build else 'yes'}")
    print("=" * 60)

    all_ok = True
    for env in envs:
        if not process_variant(version, env, args.no_build):
            all_ok = False

    print("\n" + "=" * 60)
    if all_ok:
        print("SUCCESS! Firmware files created in:", output_dir)
        print("=" * 60)
        print(f"\nWEB FLASHER USAGE:")
        print(f"  - Open: https://espressif.github.io/esptool-js/")
        print(f"  - Flash the firmware-*.bin file at offset 0x0")
        print(f"\nOTA UPDATE USAGE:")
        print(f"  - Open the device portal, Maintenance page")
        print(f"  - Upload the OTA_ONLY_firmware-*.bin in Firmware update")
        return 0
    else:
        print("ERROR: Failed to create one or more firmware files")
        print("=" * 60)
        return 1


if __name__ == '__main__':
    sys.exit(main())
