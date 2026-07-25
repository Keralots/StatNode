#!/usr/bin/env python3
"""Generate TFT_eSPI-compatible VLW font PROGMEM headers from a TTF file.

Adapted from the BambuHelper generator, with two additions: a per-font charset
so the large display-value faces can ship a digits-only subset, and a refusal to
overwrite an existing header unless --force is given (the four text fonts are
already shipped and baked into the 240x240 boards' output; regenerating them
under a different freetype build could shift rasterization).

VLW format (Bodmer/TFT_eSPI smooth font):
  Header:  glyph_count(u32) | version(u32=6) | font_size(u32) | padding(u32)
           ascent(u32) | descent(u32)
  Per glyph metrics (28 bytes each, 7 x uint32 big-endian):
           unicode(u32) | height(u32) | width(u32) | advance(u32)
           top_offset(i32) | left_offset(i32) | padding(u32, ignored by reader)
  Glyph bitmaps: 8-bit alpha, row-major, width*height bytes each.

Usage:
  python scripts/generate_vlw_fonts.py            # only missing headers
  python scripts/generate_vlw_fonts.py --force    # regenerate everything
  python scripts/generate_vlw_fonts.py --only inter_num_48
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    import freetype
except ImportError:
    print("Install freetype-py:  pip install freetype-py")
    sys.exit(1)

# Full text charset: printable ASCII + Latin-1 Supplement + Latin Extended-A
# + Romanian comma-below S/T + Euro sign. Covers DE, PL, CZ/SK, HU, TR, RO,
# FR, ES, PT, Nordic, HR/SI. CJK/emoji are out of scope (flash size).
# MUST stay sorted ascending: the VLW glyph table is looked up with a binary
# search (std::lower_bound) that assumes ascending unicode order in file order.
CHARSET_TEXT = (
    list(range(0x20, 0x7F))          # printable ASCII
    + list(range(0xA0, 0x100))       # Latin-1 Supplement (umlauts, ss, degree, ...)
    + list(range(0x100, 0x180))      # Latin Extended-A (Polish, Czech, Turkish, ...)
    + [0x218, 0x219, 0x21A, 0x21B]   # Romanian S,s,T,t with comma below
    + [0x20AC]                       # Euro sign
)

# Digits-only subset for the oversized value faces. A full charset at 48-68px
# would cost megabytes of flash, and these faces only ever render the output of
# formatMetricText()/slotProbe() in renderer.cpp, which emits digits plus '.',
# '-' (negatives and the "--" no-reading placeholder), a space, and 'k' (the
# compact RPM form, e.g. "1.8k"). Keep this in sync with that formatter, and
# NEVER use these fonts for labels or units: a missing glyph renders nothing.
# Sorted ascending, as required by the VLW binary search.
CHARSET_NUMERIC = (
    [0x20, 0x2D, 0x2E]               # space, hyphen-minus, full stop
    + list(range(0x30, 0x3A))        # 0-9
    + [0x6B]                         # 'k'
)

# Font tables to generate: (name, ttf_filename, pixel_size, charset)
# Mixed weights match TFT_eSPI bitmap fonts: Font 1/2 were medium, Font 4 bold.
FONTS = [
    ("inter_10", "Inter-Regular.ttf", 12, CHARSET_TEXT),
    ("inter_14", "Inter-Regular.ttf", 16, CHARSET_TEXT),
    ("inter_19", "Inter-Bold.ttf",    22, CHARSET_TEXT),
    # Used only on jc3248w535 (DISPLAY_320x480) for gauge primary values -
    # the larger canvas can afford a taller inscribed number without
    # crowding the secondary line below it.
    ("inter_22", "Inter-Bold.ttf",    26, CHARSET_TEXT),
    # Oversized value faces, linked on every board. Wherever a face is picked
    # to fill a cell, the alternative is magnifying a 22-26px glyph with
    # setTextSize() - that stretches the glyph's 8-bit alpha ramp and reads as
    # blurry. These render natively instead, so the digits stay crisp.
    # The four sizes are chosen against the value bands the faces actually
    # produce (fitValueFont measures fontHeight, which is ~1.22x the pixel size
    # for Inter): 30px covers the ~39px band a 240x240 panel gives a 6-metric
    # grid, 38px the ~47-61px bands of its roomier layouts, 48/68px the tall
    # cells on 320x480.
    ("inter_num_30", "Inter-Bold.ttf", 30, CHARSET_NUMERIC),
    ("inter_num_38", "Inter-Bold.ttf", 38, CHARSET_NUMERIC),
    ("inter_num_48", "Inter-Bold.ttf", 48, CHARSET_NUMERIC),
    ("inter_num_68", "Inter-Bold.ttf", 68, CHARSET_NUMERIC),
]

FONTS_DIR = Path(__file__).parent.parent / "fonts"
OUT_DIR = Path(__file__).parent.parent / "include" / "fonts"


def generate_vlw(ttf_path: str, pixel_size: int, charset) -> bytes:
    """Generate VLW binary data for the given font, size, and charset."""
    face = freetype.Face(str(ttf_path))
    face.set_pixel_sizes(0, pixel_size)

    glyphs = []
    for codepoint in charset:
        # Skip codepoints the TTF has no glyph for (e.g. U+00AD, U+0149 in
        # Inter). Emitting a .notdef box would waste flash and confuse the
        # reader; the header glyph_count below is len(glyphs), so skipping
        # keeps the count honest.
        if face.get_char_index(codepoint) == 0:
            print(f"  (skip U+{codepoint:04X}: no glyph in TTF)")
            continue
        face.load_char(chr(codepoint), freetype.FT_LOAD_RENDER)
        g = face.glyph
        bmp = g.bitmap

        width = bmp.width
        height = bmp.rows
        advance = g.advance.x >> 6  # 26.6 fixed point to pixels
        top_offset = g.bitmap_top
        left_offset = g.bitmap_left

        # Extract 8-bit alpha bitmap
        if width > 0 and height > 0:
            alpha = bytes(bmp.buffer)
            # Handle pitch != width (padding per row)
            if bmp.pitch != width:
                alpha = b""
                for row in range(height):
                    start = row * bmp.pitch
                    alpha += bytes(bmp.buffer[start:start + width])
        else:
            alpha = b""

        glyphs.append({
            "unicode": codepoint,
            "height": height,
            "width": width,
            "advance": advance,
            "top_offset": top_offset,
            "left_offset": left_offset,
            "bitmap": alpha,
        })

    # Compute font metrics
    ascent = face.size.ascender >> 6
    descent = -(face.size.descender >> 6)  # TFT_eSPI expects positive descent

    # Build VLW binary
    buf = bytearray()

    # Header (6 x uint32 big-endian)
    glyph_count = len(glyphs)
    buf += struct.pack(">I", glyph_count)
    buf += struct.pack(">I", 6)  # version
    buf += struct.pack(">I", pixel_size)
    buf += struct.pack(">I", 0)  # padding
    buf += struct.pack(">I", ascent)
    buf += struct.pack(">I", descent)

    # Glyph metrics table (28 bytes each - 7 x uint32)
    # TFT_eSPI reads: unicode, height, width, xAdvance, dY, dX, padding(ignored)
    for g in glyphs:
        buf += struct.pack(">I", g["unicode"])
        buf += struct.pack(">I", g["height"])
        buf += struct.pack(">I", g["width"])
        buf += struct.pack(">I", g["advance"])
        buf += struct.pack(">i", g["top_offset"])
        buf += struct.pack(">i", g["left_offset"])
        buf += struct.pack(">I", 0)  # padding - TFT_eSPI reads and discards

    # Glyph bitmaps (sequential, same order as metrics)
    for g in glyphs:
        buf += g["bitmap"]

    return bytes(buf)


def vlw_to_header(name: str, vlw_data: bytes, pixel_size: int,
                  glyph_note: str) -> str:
    """Convert VLW binary to a C PROGMEM header."""
    lines = [
        f"// Auto-generated VLW font: {name}",
        f"// Generated by scripts/generate_vlw_fonts.py - do not edit by hand.",
        f"// Pixel size: {pixel_size}px. Glyphs: {glyph_note}",
        f"// Size: {len(vlw_data)} bytes ({len(vlw_data) / 1024:.1f} KB)",
        "#pragma once",
        "#include <pgmspace.h>",
        "",
        f"const uint8_t {name}[] PROGMEM = {{",
    ]

    # Emit bytes, 16 per line
    for i in range(0, len(vlw_data), 16):
        chunk = vlw_data[i:i + 16]
        hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"    {hex_vals},")

    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true",
                    help="regenerate headers that already exist")
    ap.add_argument("--only", action="append", default=None,
                    help="only generate this font name (repeatable)")
    args = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)

    for name, ttf_filename, size, charset in FONTS:
        if args.only and name not in args.only:
            continue

        out_path = OUT_DIR / f"{name}.h"
        if out_path.exists() and not args.force:
            print(f"Skipping {name}: {out_path.name} already exists "
                  f"(pass --force to regenerate)")
            continue

        ttf_path = FONTS_DIR / ttf_filename
        if not ttf_path.exists():
            print(f"TTF not found: {ttf_path}")
            sys.exit(1)

        glyph_note = ("digits, '.', '-', ' ', 'k' only"
                      if charset is CHARSET_NUMERIC else "full text charset")
        print(f"Generating {name} ({ttf_filename} @ {size}px)...", end=" ")
        vlw_data = generate_vlw(str(ttf_path), size, charset)
        header = vlw_to_header(name, vlw_data, size, glyph_note)

        # Force LF line endings so generated headers are consistent across
        # platforms and don't trigger `git diff --check` whitespace warnings.
        out_path.write_bytes(header.encode("utf-8"))
        print(f"{len(vlw_data)} bytes -> {out_path}")

    print("Done.")


if __name__ == "__main__":
    main()
