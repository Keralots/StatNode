#ifndef GLASS_SURFACE_H
#define GLASS_SURFACE_H

// The glass compositor: a generated gradient backdrop, panes composed above the
// panel's colour depth and dithered on the way down to it, and an antialiased
// chart that blends into the pane it sits on.
//
// The per-pixel maths (rgbMix, rgbTo565, glassSkyAt, gcPixel, ...) stays inside
// glass_surface.cpp with the loops that use it. What crosses this header is
// coarse: whole panes, whole charts, whole backdrops. The one exception is the
// pill meter in the Duo layout, which composites a two-row bar itself and so
// needs glassPaneRow and the colour type.

#include "pc_metrics.h"   // SlotHistory
#include <LovyanGFX.hpp>

// Working colour space for the compositor: subpixel channels, 1/16 of an 8-bit
// level each. int16_t rather than uint8_t so an intermediate blend can
// overshoot without wrapping.
struct Rgb { int16_t r, g, b; };
extern const int16_t RGB_ONE;    // subpixel units per 8-bit level
extern const Rgb RGB_BLACK;

// Pane treatment. Aero glosses the face of a pane; Liquid leaves the face clear
// and lights its edges. glassAero()/glassLiquid() fold the user's colourway and
// highlight sliders in.
struct GlassStyle {
  // Signed and 16-bit: the amounts run past 127 (Liquid's top rim is 132) and
  // a light colourway needs them negative.
  int16_t lift;      // pane body toward white (the "smoke"), < 0 = smoked dark
  uint8_t tint;      // slot accent bled into the body
  uint8_t bloom;     // accent glow rising off the bottom edge
  uint8_t glossA;    // specular peak alpha, 0 = no gloss at all
  uint8_t glossPct;  // specular band height as a % of pane height
  uint8_t glossBow;  // gloss taken back at the pane's sides, 0 = flat bar
  uint8_t refract;   // rim samples the sky this many rows lower, 0 = off
  int16_t rimTop;    // all three rims: > 0 lifts toward white,
  int16_t rimSide;   // < 0 sinks toward black
  int16_t rimBot;
};

// Compose target: either a sprite (blitted whole) or the panel itself.
struct GlassCanvas {
  lgfx::LovyanGFX* g;
  uint16_t* buf;       // direct sprite-buffer access when the byte order is known
  int32_t stride;
  int16_t w, h;
  int16_t ox, oy;      // where this canvas sits, for the generic path
};
GlassCanvas glassCanvasFor(lgfx::LovyanGFX& g, lgfx::LGFX_Sprite* spr,
                           int16_t ox, int16_t oy);

// Backdrop tints for the two glass recipes.
extern const Rgb GLASS_TINT_AERO;
extern const Rgb GLASS_TINT_LIQUID;

// The sky ramp is a per-frame prerequisite: every pane row reads it.
void glassSkyInit(int16_t h, const Rgb& tint,
                  uint8_t topA, uint8_t midA, uint8_t botA);
Rgb  glassSkyAt(int16_t y);
void glassBackdrop(int16_t w, int16_t h);

GlassStyle glassAero();
GlassStyle glassLiquid();

// Pane body colour for one row. A pure function of the panel row, which is what
// lets two sprites compose adjacent slices of one pane and join invisibly.
Rgb glassPaneRow(int16_t panelY, int16_t dy, int16_t paneH, const Rgb& accent,
                 const GlassStyle& gs);

// Compose a WINDOW of a pane. warnRim, when set, lights the whole edge in the
// warning colour instead of retinting the body.
void glassPaneWindow(const GlassCanvas& c, int16_t wx, int16_t wy, int16_t ww,
                     int16_t wh, int16_t paneY, int16_t paneW, int16_t paneH,
                     int16_t r, const Rgb& accent, const GlassStyle& gs,
                     const Rgb* warnRim = nullptr);

void glassChart(const GlassCanvas& c, const SlotHistory& hist,
                uint8_t slotIdx, int16_t ox, int16_t oy,
                int16_t w, int16_t h,
                const Rgb& paneAccent, const Rgb& lineAccent,
                int16_t paneY, int16_t paneLocalY,
                int16_t paneW, int16_t paneH, int16_t fadeIn,
                const GlassStyle& gs, bool advance, uint16_t scrollQ8);

// Colour conversion, needed wherever a caller mixes its own pixels.
Rgb rgbFrom565(uint16_t c);
uint16_t rgbTo565(const Rgb& c, int16_t x, int16_t y);
Rgb rgbMix(const Rgb& a, const Rgb& b, uint8_t alpha);

// Text inks for the current colourway.
uint16_t glassLabelInk(uint16_t accent565);
uint16_t glassValueInk(bool warn);
uint16_t glassUnitInk();

#endif // GLASS_SURFACE_H
