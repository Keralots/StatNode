#ifndef FONTS_H
#define FONTS_H

#include <LovyanGFX.hpp>

// Smooth VLW font identifiers - values match the old bitmap font numbers
// so existing logic (e.g. compact ? FONT_LARGE : FONT_LARGE) stays readable.
enum FontID : uint8_t {
    FONT_NONE   = 0,
    FONT_SMALL  = 1,   // Inter 10pt  (was bitmap Font 1, 8px GLCD)
    FONT_BODY   = 2,   // Inter 14pt  (was bitmap Font 2, 16px)
    FONT_LARGE  = 4,   // Inter 19pt  (was bitmap Font 4, 26px)
    FONT_XLARGE = 5,   // Inter 22pt - only loaded on DISPLAY_320x480; on
                       // other boards setFont(FONT_XLARGE) silently falls
                       // back to FONT_LARGE (the inter_22 blob isn't linked).
    FONT_7SEG   = 7,   // Built-in 7-segment (kept for clock displays)

    // Oversized value faces, DIGITS ONLY - see CHARSET_NUMERIC in
    // scripts/generate_vlw_fonts.py. They carry '0'-'9', '.', '-', ' ' and 'k'
    // and NOTHING else, so they must only ever render formatMetricText() /
    // slotProbe() output. Drawing a label or unit with one renders nothing for
    // the missing glyphs. Like FONT_XLARGE they are only linked on
    // DISPLAY_320x480 and fall back to FONT_LARGE elsewhere.
    //
    // They exist because the 320x480 grid faces get ~160px-tall cells, where
    // the 26px FONT_XLARGE had to be magnified ~2x via setTextSize(). That
    // stretches the glyph's 8-bit alpha ramp and reads as blurry on the panel;
    // rendering natively at the target size keeps the digits crisp.
    FONT_NUM_XL  = 8,  // Inter Bold 48px, digits only
    FONT_NUM_XXL = 9,  // Inter Bold 68px, digits only
};

// Sets the active font. Caches the last selection - calling setFont() repeatedly
// with the same id is a no-op. If a VLW font fails to load (e.g. heap exhausted)
// the helper falls back to a built-in bitmap font of similar height instead of
// silently leaving the previous font (or Font0) selected.
// Note: parameter is named `gfx` (not `tft`) because display_ui.h defines
// a `#define tft (*tft_ptr)` macro for the JC3248W535 sprite redirection,
// which would otherwise mangle this declaration when both headers are
// included in the same translation unit.
void setFont(lgfx::LovyanGFX& gfx, FontID id);

// Invalidate the remembered font selection (it is global, not per-device).
// Call after switching the render target so setFont() reloads on the new one.
void resetFontCache();

#endif // FONTS_H
