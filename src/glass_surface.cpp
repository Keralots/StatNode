// The glass compositor. See glass_surface.h for what crosses the boundary and
// why the per-pixel helpers deliberately do not.
#include "glass_surface.h"
#include "renderer_internal.h"
#include <math.h>

// ---------------------------------------------------------------------------
//  GLASS SURFACE KIT  (STYLE_GLASS_TILES / STYLE_GLASS_DUO)
//
//  Real glass wants a blurred backdrop and this panel has no framebuffer to
//  blur. It does not need one: a smooth vertical gradient, blurred, is still
//  itself. So the wallpaper behind the glass is a gradient the firmware
//  generates, which makes the "backdrop blur" exact rather than approximate -
//  and makes every pane colour a pure function of the panel row, so a pane can
//  be composed in sprite-sized pieces that join seamlessly and never has to
//  read the panel back.
//
//  Three rules hold the whole thing together:
//   1. The backdrop is vertical-only. A diagonal one would look better and
//      would cost a per-pixel evaluation instead of one value per row.
//   2. Compositing happens at 8 bits per channel and is dithered on the way
//      down to RGB565. Blending in 565 space bands badly - a 240-row ramp
//      crosses at most 32 blue levels, so straight truncation lays down
//      visible horizontal stripes.
//   3. Nothing direct-draws onto glass. The "clear the vacated pixels to
//      bgColor" trick the flat faces use dies the moment bgColor stops being
//      one colour, so every region that updates is composed and blitted whole.
// ---------------------------------------------------------------------------

// Working colour space for the compositor: SUBPIXEL channels, 1/16 of an 8-bit
// level each, so 0..RGB_ONE*255. int16_t rather than uint8_t so an intermediate
// blend can overshoot without wrapping.
//
// The fraction is the whole point. Every gradient here is slow - a card is ~115
// rows and its colour travels maybe 14 levels over that - so at plain 8 bits the
// COMPOSITE was already a staircase before the dither ever saw it: eight rows of
// one integer, a jump, eight more. The dither can only stipple between the two
// 565 levels it is handed, so it reproduced that staircase faithfully and the
// panel showed one contour line per 8-bit level. Carrying four fractional bits
// makes the ramp advance every row and the dither turns it into a true gradient.
// Measured on a 320x480 Glass Tiles card: longest flat run 13 rows -> 3.
extern const int16_t RGB_ONE = 16;                // subpixel units per level
static const int16_t RGB_MAX = 255 * RGB_ONE;     // 4080


Rgb rgbFrom565(uint16_t c) {
  return Rgb{ (int16_t)((((c >> 11) & 0x1F) * RGB_MAX + 15) / 31),
              (int16_t)((((c >> 5) & 0x3F) * RGB_MAX + 31) / 63),
              (int16_t)(((c & 0x1F) * RGB_MAX + 15) / 31) };
}

// alpha is the weight of b, 0..255.
Rgb rgbMix(const Rgb& a, const Rgb& b, uint8_t alpha) {
  return Rgb{ (int16_t)(a.r + (((int32_t)b.r - a.r) * alpha >> 8)),
              (int16_t)(a.g + (((int32_t)b.g - a.g) * alpha >> 8)),
              (int16_t)(a.b + (((int32_t)b.b - a.b) * alpha >> 8)) };
}

// Same blend with the alpha itself in 1/256ths, for the ramps whose alpha
// crawls: gloss and bloom move by well under one alpha step per row, and an
// integer alpha there re-introduces exactly the staircase the subpixel colour
// space exists to remove. Full weight is 255 << 8.
static inline Rgb rgbMixQ8(const Rgb& a, const Rgb& b, int32_t alphaQ8) {
  return Rgb{ (int16_t)(a.r + (((int32_t)b.r - a.r) * alphaQ8 >> 16)),
              (int16_t)(a.g + (((int32_t)b.g - a.g) * alphaQ8 >> 16)),
              (int16_t)(a.b + (((int32_t)b.b - a.b) * alphaQ8 >> 16)) };
}
static const int32_t ALPHA_FULL_Q8 = 255 << 8;

static const Rgb RGB_WHITE = { RGB_MAX, RGB_MAX, RGB_MAX };
extern const Rgb RGB_BLACK = { 0, 0, 0 };

// 8x8 ordered dither, applied at the subpixel -> 565 store so a long ramp turns
// its band edges into a stipple the eye integrates back into a smooth
// gradient. Without it the backdrop shows every one of its ~32 blue steps.
//
// 8x8 rather than 4x4: both kill the banding, but a 4x4 cell at this pixel
// pitch reads as a visible checkerboard on the panel.
static const uint8_t kBayer8[64] = {
   0, 32,  8, 40,  2, 34, 10, 42,
  48, 16, 56, 24, 50, 18, 58, 26,
  12, 44,  4, 36, 14, 46,  6, 38,
  60, 28, 52, 20, 62, 30, 54, 22,
   3, 35, 11, 43,  1, 33,  9, 41,
  51, 19, 59, 27, 49, 17, 57, 25,
  15, 47,  7, 39, 13, 45,  5, 37,
  63, 31, 55, 23, 61, 29, 53, 21
};

// The threshold is ADDED, never centred. Ordered dither quantises by TRUNCATION,
// so the spread has to run 0..(step-1) for the expected output to come back
// equal to the input; a centred -32..31 threshold instead shifts every channel
// half a quantisation step DOWN (measured: -3.2/255 of blue across the backdrop)
// and leaves the top half of each bucket unreachable. One 565 step is 8 levels
// of red/blue and 4 of green, which in subpixel units is 128 and 64 - hence the
// 0..126 and 0..63 spreads below.
uint16_t rgbTo565(const Rgb& c, int16_t x, int16_t y) {
  const int32_t t = (int32_t)kBayer8[((y & 7) << 3) | (x & 7)];
  int32_t r = (c.r + (t << 1)) >> 7;
  int32_t g = (c.g + t) >> 6;
  int32_t b = (c.b + (t << 1)) >> 7;
  if (r < 0) r = 0; else if (r > 31) r = 31;
  if (g < 0) g = 0; else if (g > 63) g = 63;
  if (b < 0) b = 0; else if (b > 31) b = 31;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Compose target. Writing straight into a sprite's buffer instead of going
// through drawPixel is what makes a fully per-pixel glass face affordable on
// the C3, which has no FPU and pays for every call. buf == nullptr falls back
// to the generic path so the no-sprite case still renders.

// LovyanGFX keeps 16-bit sprite pixels in the PANEL's byte order (swap565 on
// every SPI panel here), and writing the buffer directly bypasses the
// conversion the drawing API would do - which comes out as scrambled hues, not
// as a subtle shift. Probe once with an asymmetric colour instead of assuming
// an order: the answer differs by panel driver, and guessing wrong is silent.
static int8_t gGlassSwap = -1;   // -1 = not yet probed, 0 = native, 1 = swapped

static void glassProbeByteOrder(lgfx::LGFX_Sprite& spr) {
  if (gGlassSwap >= 0) return;
  const uint16_t probe = 0xF800;            // pure red: 0x00F8 once swapped
  spr.drawPixel(0, 0, probe);
  gGlassSwap = (((const uint16_t*)spr.getBuffer())[0] == probe) ? 0 : 1;
}

GlassCanvas glassCanvasFor(lgfx::LovyanGFX& g, lgfx::LGFX_Sprite* spr,
                                  int16_t ox, int16_t oy) {
  GlassCanvas c;
  c.g = &g;
  c.buf = nullptr;
  c.stride = 0;
  c.w = (int16_t)g.width();
  c.h = (int16_t)g.height();
  c.ox = ox;
  c.oy = oy;
  if (spr && (int)spr->getColorDepth() == 16 && spr->getBuffer()) {
    glassProbeByteOrder(*spr);
    c.buf = (uint16_t*)spr->getBuffer();
    c.stride = (int32_t)spr->width();
  }
  return c;
}

static inline void gcPixel(const GlassCanvas& c, int16_t x, int16_t y, uint16_t v) {
  const int16_t px = x + c.ox, py = y + c.oy;
  if (px < 0 || py < 0 || px >= c.w || py >= c.h) return;
  if (c.buf) c.buf[(int32_t)py * c.stride + px] = gGlassSwap ? __builtin_bswap16(v) : v;
  else c.g->drawPixel(px, py, v);
}

// --- the backdrop -----------------------------------------------------------

// Widest panel this firmware targets (the 320x480 Guition in landscape).
static const int16_t GLASS_ROW_MAX = 480;

struct GlassSky {
  Rgb top, mid, bot;
  int16_t h;
};
static GlassSky gSky;

// --- colourways --------------------------------------------------------------
//
// Ported from the Home Assistant Frosted Glass themes
// (github.com/wessamlauf/homeassistant-frosted-glass-themes, MIT). What carries
// over is the PALETTE, not the mechanism. That theme frosts a photographic
// wallpaper with a real backdrop-filter blur; this panel has no framebuffer to
// blur, and the gradient-as-backdrop trick only survives because a blurred
// smooth ramp is still itself. So the ramps below are the theme's own primary
// scale, which is roughly what its blurred wallpaper averages out to, and the
// pane numbers are its card tint and inset box-shadows retuned for a 1.75px
// soft rim instead of a 3px hard bevel.
static inline Rgb rgb8(int16_t r, int16_t g, int16_t b) {
  return Rgb{ (int16_t)(r * RGB_ONE), (int16_t)(g * RGB_ONE),
              (int16_t)(b * RGB_ONE) };
}

struct GlassTheme {
  bool    fixed;      // false: derive the ramp from the user's bgColor
  Rgb     top, mid, bot;
  int16_t lift;       // overrides GlassStyle.lift
  uint8_t tint;       // overrides GlassStyle.tint
  uint8_t bloom;      // overrides GlassStyle.bloom
  int16_t rimTop, rimSide, rimBot;
  Rgb     inkBase;    // glassLabelInk mixes this toward the slot accent
  Rgb     valueInk;
  Rgb     unitInk;
};

// Frosted Dark: the theme's primary-30/20/05 as the ramp, a DARK card tint
// (rgba(28,29,33,0.18) over the backdrop, hence a negative lift) and the bright
// thin edge its stacked white insets and rgba(234,235,238,0.22) border produce.
// Deep-frost glass reads by its edges, not by its face.
// The ramp stops at primary-20 rather than running down to primary-05: the
// two-segment sky already spends its lower 45% heading for the floor, and with
// a near-black floor the bottom half of the panel crushed flat and the lowest
// cards had nothing left to sit on. This theme's wallpaper is a mid navy, not
// a black one.
static const GlassTheme GLASS_THEME_DARK_DEF = {
  /*fixed*/ true,
  /*top*/ rgb8(0x40, 0x46, 0x7F), /*mid*/ rgb8(0x30, 0x34, 0x5F),
  /*bot*/ rgb8(0x19, 0x1C, 0x2D),       // token-color-background-base
  // Accent bleed and bloom are both well under the default face's: a negative
  // lift makes the pane dark, and the SAME tint that reads as a hint on smoked
  // glass reads as a solid colour block on a dark one. Frosted glass is neutral
  // and takes its identity from the label and the meter, not from the body.
  /*lift*/ -22, /*tint*/ 10, /*bloom*/ 8,
  /*rimTop*/ 96, /*rimSide*/ 60, /*rimBot*/ 30,
  /*inkBase*/ rgb8(0xEA, 0xEB, 0xEE),   // token-color-text-primary
  /*valueInk*/ rgb8(0xF6, 0xF7, 0xFC),
  /*unitInk*/ rgb8(0xAD, 0xB3, 0xE7)    // primary-70, the theme's soft accent
};

// Frosted Light: primary-95/90/80. The pane lifts toward white, but what
// actually separates a pale card from a pale ground is the SHADOW, so the side
// and bottom rims go negative - that is the theme's
// box-shadow: 0 12px 20px rgba(0,0,0,0.15). A white rim there would be invisible.
static const GlassTheme GLASS_THEME_LIGHT_DEF = {
  /*fixed*/ true,
  // The ramp sits a step DOWN the scale from where the panes land. A near-white
  // backdrop under near-white panes is what made the first pass read as one
  // flat sheet with the cards missing, so the wallpaper takes primary-90..80
  // and the pane lifts clear of it.
  /*top*/ rgb8(0xEA, 0xEC, 0xF9), /*mid*/ rgb8(0xD2, 0xD5, 0xF2),
  /*bot*/ rgb8(0xAD, 0xB3, 0xE7),
  /*lift*/ 78, /*tint*/ 12, /*bloom*/ 10,
  /*rimTop*/ 70, /*rimSide*/ -40, /*rimBot*/ -95,
  /*inkBase*/ rgb8(0x13, 0x15, 0x36),   // token-color-text-primary
  /*valueInk*/ rgb8(0x13, 0x15, 0x36),
  /*unitInk*/ rgb8(0x41, 0x43, 0x5F)    // the same at 0.8 over a light ground
};

static const GlassTheme& glassThemeNow() {
  switch (glassTheme) {
    case GLASS_THEME_FROSTED_DARK:  return GLASS_THEME_DARK_DEF;
    case GLASS_THEME_FROSTED_LIGHT: return GLASS_THEME_LIGHT_DEF;
    default: break;
  }
  static const GlassTheme none = {};   // fixed == false
  return none;
}

// True while a pale colourway is active. Everything that brightens a mark to
// separate it from the pane has to invert here: on a white pane, lifting a
// colour toward white is how a mark DISAPPEARS.
static inline bool glassLightMode() {
  return glassTheme == GLASS_THEME_FROSTED_LIGHT;
}

// Label ink on glass: mostly the theme's text colour with enough of the slot
// accent to identify the series, which survives the lifted pane where a dim
// grey would not.
uint16_t glassLabelInk(uint16_t accent565) {
  const GlassTheme& t = glassThemeNow();
  const Rgb base = t.fixed ? t.inkBase : RGB_WHITE;
  const Rgb c = rgbMix(base, rgbFrom565(accent565), 88);
  return rgbTo565(c, 0, 0);
}

// Reading and unit. A Frosted preset owns these: the theme text colours are
// authored against the user's own bgColor, and a light colourway with the
// default near-white value ink would put white digits on a white pane.
// The warning colour is NOT overridden - a warning has to keep shouting.
uint16_t glassValueInk(bool warn) {
  if (warn) return dispSettings.warnColor;
  const GlassTheme& t = glassThemeNow();
  return t.fixed ? rgbTo565(t.valueInk, 0, 0) : themeSettings.valueColor;
}

uint16_t glassUnitInk() {
  const GlassTheme& t = glassThemeNow();
  return t.fixed ? rgbTo565(t.unitInk, 0, 0) : themeSettings.secondaryColor;
}

// Default: derived from the user's bgColor so the Colors card still means
// something - the ramp is that colour lifted toward a cool slate at the top and
// sunk toward black at the bottom. A Frosted preset replaces it outright, which
// is why selecting one makes bgColor stop applying to the glass faces.
void glassSkyInit(int16_t h, const Rgb& tint,
                         uint8_t topA, uint8_t midA, uint8_t botA) {
  gSky.h = h > 1 ? h : 1;
  const GlassTheme& t = glassThemeNow();
  if (t.fixed) {
    gSky.top = t.top; gSky.mid = t.mid; gSky.bot = t.bot;
    return;
  }
  const Rgb base = rgbFrom565(dispSettings.bgColor);
  gSky.top = rgbMix(base, tint, topA);
  gSky.mid = rgbMix(base, tint, midA);
  gSky.bot = rgbMix(base, RGB_BLACK, botA);
}

// Two-segment ramp: the knee at 55% keeps the upper half bright enough for the
// panes to read as sitting ON something while the floor still goes properly
// dark behind the lowest cards.
// The interpolation weight is Q8 for the same reason the colour is subpixel: a
// 480-row backdrop advances well under one alpha step per row, and rounding it
// to a whole step is what put horizontal contour lines across the wallpaper.
Rgb glassSkyAt(int16_t y) {
  if (y < 0) y = 0;
  if (y >= gSky.h) y = gSky.h - 1;
  const int32_t knee = ((int32_t)gSky.h * 55) / 100;
  if (y <= knee) {
    const int32_t a = knee ? ((int32_t)y * ALPHA_FULL_Q8) / knee : 0;
    return rgbMixQ8(gSky.top, gSky.mid, a);
  }
  const int32_t span = gSky.h - 1 - knee;
  const int32_t a = span > 0 ? (((int32_t)y - knee) * ALPHA_FULL_Q8) / span
                             : ALPHA_FULL_Q8;
  return rgbMixQ8(gSky.mid, gSky.bot, a);
}

// One dithered row at a time, pushed as an image. drawFastHLine would be one
// call per row and would band; per-pixel drawPixel would be 57600 windowed SPI
// writes and take seconds. Composing the row in RAM and blitting it is both
// smooth and fast.
void glassBackdrop(int16_t w, int16_t h) {
  static uint16_t row[GLASS_ROW_MAX];
  if (w > GLASS_ROW_MAX) w = GLASS_ROW_MAX;
  RendererWrite rw(tft);
  // row[] holds native-order RGB565, and pushImage's flag means "swap this
  // source into the panel's order" - which these swap565 panels need. Setting
  // it false publishes the row as already-panel-order and scrambles every hue
  // (measured: a #19AA navy backdrop came back as #AA19 magenta).
  const bool prevSwap = tft.getSwapBytes();
  tft.setSwapBytes(true);
  for (int16_t y = 0; y < h; y++) {
    const Rgb c = glassSkyAt(y);
    for (int16_t x = 0; x < w; x++) row[x] = rgbTo565(c, x, y);
    tft.pushImage(0, y, w, 1, row);
  }
  tft.setSwapBytes(prevSwap);
}

// --- pane -------------------------------------------------------------------

// Shade toward white for a positive amount, toward black for a negative one.
// Every surface term uses this rather than a bare mix toward RGB_WHITE, because
// a light colourway needs the exact opposite polarity from a dark one: on a
// pale backdrop a white rim is invisible and it is the SHADOW under the pane
// that separates the card from its ground.
static inline Rgb rgbShade(const Rgb& c, int16_t amt) {
  if (amt >= 0) return rgbMix(c, RGB_WHITE, (uint8_t)(amt > 255 ? 255 : amt));
  const int16_t a = (int16_t)(-amt);
  return rgbMix(c, RGB_BLACK, (uint8_t)(a > 255 ? 255 : a));
}


// Vista: polished. A bright specular arc, a hard bevel underneath, warm bloom.
// glossA and glossBow are the portal's two Glass surface sliders, so this is a
// template - take a copy through glassAero() rather than using it directly.
static const GlassStyle GLASS_AERO = {
  /*lift*/ 26, /*tint*/ 30, /*bloom*/ 30, /*glossA*/ 80, /*glossPct*/ 44,
  /*glossBow*/ 40, /*refract*/ 0, /*rimTop*/ 120, /*rimSide*/ 34, /*rimBot*/ -76
};
// Modern: edge-lit, no gloss, light wrapping under the bottom edge. The
// highlight sliders do not reach here on purpose: this face's whole identity is
// that its edges are lit and its FACE is not.
static const GlassStyle GLASS_LIQUID = {
  /*lift*/ 30, /*tint*/ 26, /*bloom*/ 18, /*glossA*/ 0, /*glossPct*/ 0,
  /*glossBow*/ 0, /*refract*/ 6, /*rimTop*/ 132, /*rimSide*/ 72, /*rimBot*/ 54
};

// Fold the active colourway into a face's surface template. The gloss terms are
// handled by the caller: they are the face's identity plus the user's two
// sliders, not something the palette owns.
static GlassStyle glassThemed(GlassStyle gs) {
  const GlassTheme& t = glassThemeNow();
  if (!t.fixed) return gs;
  gs.lift = t.lift;
  gs.tint = t.tint;
  gs.bloom = t.bloom;
  gs.rimTop = t.rimTop;
  gs.rimSide = t.rimSide;
  gs.rimBot = t.rimBot;
  return gs;
}

// Portal preview override. It shadows the persisted settings rather than
// writing them, so a slider drag costs no NVS write and /api/config still
// reports what is stored - which is what lets Revert put the panel back.
static bool gGlassPreview = false;
static uint8_t gGlassPvGloss = 0, gGlassPvBow = 0, gGlassPvFill = 0;

void setGlassPreview(bool on, uint8_t glossPct, uint8_t bowPct, uint8_t fillPct) {
  gGlassPreview = on;
  gGlassPvGloss = glossPct;
  gGlassPvBow = bowPct;
  gGlassPvFill = fillPct;
}

static inline uint8_t glassGlossNow() {
  return gGlassPreview ? gGlassPvGloss : glassGlossPct;
}
static inline uint8_t glassBowNow() {
  return gGlassPreview ? gGlassPvBow : glassBowPct;
}
static inline uint8_t glassChartFillNow() {
  return gGlassPreview ? gGlassPvFill : glassChartFillPct;
}

// Aero with the user's highlight settings folded in. Both sliders are stored as
// percentages so the portal can show them as percentages.
GlassStyle glassAero() {
  GlassStyle gs = glassThemed(GLASS_AERO);
  gs.glossA   = (uint8_t)(((uint16_t)glassGlossNow() * 255) / 100);
  gs.glossBow = (uint8_t)(((uint16_t)glassBowNow() * 255) / 100);
  // A white specular on a pale pane is not a highlight, it is a washed-out
  // patch, so the light colourway keeps only a third of the slider's strength.
  if (glassTheme == GLASS_THEME_FROSTED_LIGHT) gs.glossA = (uint8_t)(gs.glossA / 3);
  return gs;
}

// Liquid takes the colourway but nothing else: its identity is that its edges
// are lit and its face is not, so it has no gloss for a slider to reach.
GlassStyle glassLiquid() {
  return glassThemed(GLASS_LIQUID);
}

// Pane body colour for one row. Pure function of the panel row, which is what
// lets two sprites compose adjacent slices of the same pane and join invisibly.
Rgb glassPaneRow(int16_t panelY, int16_t dy, int16_t paneH,
                               const Rgb& accent, const GlassStyle& gs) {
  Rgb c = rgbShade(glassSkyAt(panelY), gs.lift);
  c = rgbMix(c, accent, gs.tint);
  if (gs.bloom && paneH > 1) {
    const int32_t t = ((int32_t)dy * ALPHA_FULL_Q8) / (paneH - 1);
    if (t > (140 << 8))
      c = rgbMixQ8(c, accent, ((int32_t)gs.bloom * (t - (140 << 8))) / 115);
  }
  return c;
}

// Sub-pixel left inset of a rounded-rect row, in 1/256 px. The fractional part
// is what removes the staircase from a 6px corner - an integer inset is
// exactly the "sharp edges" problem at this radius.
static int32_t glassInsetQ8(int16_t r, int16_t dy, int16_t paneH) {
  if (r <= 0) return 0;
  const int16_t d = (dy < paneH - 1 - dy) ? dy : (int16_t)(paneH - 1 - dy);
  if (d >= r) return 0;
  const float k = (float)r - ((float)d + 0.5f);
  float s = (float)r * (float)r - k * k;
  if (s < 0.0f) s = 0.0f;
  const float ins = (float)r - sqrtf(s);
  return (int32_t)(ins * 256.0f + 0.5f);
}

// Soft edge widths, in 1/256 px. Wide enough that the rim reads as a lit bevel
// rather than a drawn outline.
static const int32_t RIM_SIDE_Q8 = 384;   // 1.5 px
static const int32_t RIM_VERT_Q8 = 448;   // 1.75 px

// Compose a WINDOW of a glass pane. The window is [wx, wy, ww, wh] in
// pane-local coordinates; the pane is paneW x paneH with corner radius r and
// its top row sits at panel row paneY. Everything outside the rounded shape
// gets the backdrop, so a window is self-contained and two adjacent windows
// tile the pane exactly.
// warnRim, when set, lights the pane's whole edge in the warning colour. The
// alternative - retinting the pane body - is what made a slot crossing its
// threshold look broken: the body went red while the chart inside kept its
// identity hue, so the plot read as a mismatched rectangle stamped on the
// card. Every other face here keeps the card its identity colour too.
void glassPaneWindow(const GlassCanvas& c,
                            int16_t wx, int16_t wy, int16_t ww, int16_t wh,
                            int16_t paneY, int16_t paneW, int16_t paneH,
                            int16_t r, const Rgb& accent, const GlassStyle& gs,
                            const Rgb* warnRim) {
  const int16_t glossH = gs.glossA ? (int16_t)(((int32_t)paneH * gs.glossPct) / 100) : 0;
  const int32_t cxQ8 = ((int32_t)paneW << 8) / 2;

  for (int16_t dy = wy; dy < wy + wh; dy++) {
    const int16_t panelRow = paneY + dy;
    const Rgb sky = glassSkyAt(panelRow);
    const Rgb body = glassPaneRow(panelRow, dy, paneH, accent, gs);

    const int32_t insQ8 = glassInsetQ8(r, dy, paneH);
    const int32_t xlQ8 = insQ8;
    const int32_t xrQ8 = ((int32_t)paneW << 8) - insQ8;

    // Rim source: Liquid reads the backdrop from further down the panel before
    // lifting it, which is what light bending through a thick edge looks like.
    // The refracted rim source is lifted a little further than the body in
    // whichever direction the body itself was shaded, so the sign follows lift.
    const Rgb rimSrc = gs.refract
      ? rgbShade(glassSkyAt(panelRow + gs.refract),
                 gs.lift >= 0 ? (int16_t)(gs.lift + 24) : (int16_t)(gs.lift - 24))
      : body;
    Rgb rimTopC  = rgbShade(rimSrc, gs.rimTop);
    Rgb rimSideC = rgbShade(rimSrc, gs.rimSide);
    Rgb rimBotC  = rgbShade(body, gs.rimBot);
    if (warnRim) {
      rimTopC  = rgbMix(rimTopC,  *warnRim, 200);
      rimSideC = rgbMix(rimSideC, *warnRim, 210);
      rimBotC  = rgbMix(rimBotC,  *warnRim, 170);
    }

    // Vertical rim weights are constant across the row.
    const int32_t evT = ((int32_t)dy << 8) + 128;
    const int32_t evB = ((int32_t)(paneH - 1 - dy) << 8) + 128;
    int32_t wTop = evT < RIM_VERT_Q8 ? ((RIM_VERT_Q8 - evT) * 255) / RIM_VERT_Q8 : 0;
    int32_t wBot = evB < RIM_VERT_Q8 ? ((RIM_VERT_Q8 - evB) * 255) / RIM_VERT_Q8 : 0;

    // Specular height for this row is a per-column parabola; precompute the
    // row's falloff factor once.
    int32_t glossF = 0;                             // Q8, 0..ALPHA_FULL_Q8
    if (glossH > 0 && dy < glossH) {
      const int32_t f = ALPHA_FULL_Q8 - ((int32_t)dy * ALPHA_FULL_Q8) / glossH;
      // f*f would overflow int32 at full scale; taking the high byte of one
      // factor keeps the square in range and still resolves 1/255 of the ramp.
      glossF = ((f >> 8) * f) / 255;                // quadratic, no hard edge
    }

    // Fast interior. Away from the corner curve, the rim and the specular, a
    // pane row is one flat colour and only the ordered dither varies - and the
    // dither repeats every 8 columns. Precomputing those 8 values turns the
    // bulk of every pane into an array read plus a store, instead of six
    // rgbMix chains per pixel. This is the difference between a 49 ms Glass
    // Tiles frame and one that fits the budget.
    const bool plainRow = (insQ8 == 0) && (wTop == 0) && (wBot == 0) && (glossF == 0);
    uint16_t dith[8];
    if (plainRow) for (int16_t i = 0; i < 8; i++) dith[i] = rgbTo565(body, i, dy);
    const int16_t edge = 3;   // widest the side rim can reach

    const int16_t x0 = wx, x1 = wx + ww;
    for (int16_t x = x0; x < x1; x++) {
      if (plainRow && x >= edge && x < paneW - edge) {
        gcPixel(c, x, dy, dith[x & 7]);
        continue;
      }
      const int32_t pxL = ((int32_t)x << 8), pxC = pxL + 128;

      // Horizontal coverage of the rounded shape by this pixel.
      int32_t a = pxL > xlQ8 ? pxL : xlQ8;
      int32_t b = (pxL + 256) < xrQ8 ? (pxL + 256) : xrQ8;
      int32_t cov = b - a;
      if (cov <= 0) { gcPixel(c, x, dy, rgbTo565(sky, x, dy)); continue; }
      if (cov > 256) cov = 256;

      Rgb col = body;

      if (glossF > 0) {
        // Gloss bowed toward the pane's centre column. glossBow is how much of
        // the highlight is taken back at the sides: at the old 140 the centre
        // of a short wide card was 34% white while its corners were 15%, and
        // since a tile's label sits at one end and its value at the other, that
        // bright core landed in the empty gap BETWEEN them - read as a smudge on
        // the background rather than as a highlight on the glass. A near-flat
        // bar keeps the Vista sheen and drops the blotch.
        const int32_t t = ((pxC - cxQ8) * 255) / (cxQ8 > 0 ? cxQ8 : 1);
        int32_t shape = 255 - (t * t * gs.glossBow) / (255 * 255);
        if (shape > 0) {
          const int32_t al = ((int32_t)gs.glossA * glossF / 255) * shape / 255;
          if (al > 0)
            col = rgbMixQ8(col, RGB_WHITE, al > ALPHA_FULL_Q8 ? ALPHA_FULL_Q8 : al);
        }
      }

      // Rim: bottom first, then sides, then top - the top edge always wins,
      // which is what reads as a light source above the panel.
      const int32_t dl = pxC - xlQ8, dr = xrQ8 - pxC;
      const int32_t e = dl < dr ? dl : dr;
      const int32_t wSide = e < RIM_SIDE_Q8 ? ((RIM_SIDE_Q8 - e) * 255) / RIM_SIDE_Q8 : 0;
      if (wBot  > 0) col = rgbMix(col, rimBotC,  (uint8_t)wBot);
      if (wSide > 0) col = rgbMix(col, rimSideC, (uint8_t)wSide);
      if (wTop  > 0) col = rgbMix(col, rimTopC,  (uint8_t)wTop);

      // Antialiased outer edge against the backdrop.
      if (cov < 256) col = rgbMix(sky, col, (uint8_t)cov);
      gcPixel(c, x, dy, rgbTo565(col, x, dy));
    }
  }
}



// Screen-space Q8 y for one chart column, clamped into the plot.
static inline int32_t glassChartY(const float* s, int n, float fi,
                                  float lo, float span, int32_t hQ8) {
  const float last = (float)(n - 1);
  if (fi > last) fi = last;
  if (fi < 0.0f) fi = 0.0f;
  const float v = histSmooth(s, n, fi);
  int32_t y = hQ8 - (int32_t)(((v - lo) / span) * (float)hQ8);
  if (y < 0) y = 0;
  if (y > hQ8) y = hQ8;
  return y;
}

// Antialiased chart on glass: a vertical gradient area fill that blends into
// the pane instead of punching a flat dim block through it, under a soft 2px
// stroke. Every edge is coverage-blended, so nothing here has a hard step.
// scrollQ8 slides the sample window by a fraction of one sample so the chart
// glides between packets rather than jumping a whole column.
// paneAccent reproduces the pane exactly under the plot; lineAccent is the
// series ink. They differ whenever a slot is in warn - the pane goes warn
// coloured while the chart keeps its identity hue - and using one for both is
// what makes the plot show up as a lighter rectangle stamped on the card.
// fadeIn softens the fill's leading edge over that many columns, so a chart
// that starts mid-pane has no hard vertical seam.
void glassChart(const GlassCanvas& c, const SlotHistory& hist,
                       uint8_t slotIdx, int16_t ox, int16_t oy,
                       int16_t w, int16_t h,
                       const Rgb& paneAccent, const Rgb& lineAccent,
                       int16_t paneY, int16_t paneLocalY,
                       int16_t paneW, int16_t paneH, int16_t fadeIn,
                       const GlassStyle& gs, bool advance, uint16_t scrollQ8) {
  float lo, hi;
  if (w < 6 || h < 6) return;
  if (!sparkBounds(hist, slotIdx, advance, lo, hi)) return;
  const float span = (hi - lo) > 1e-6f ? (hi - lo) : 1.0f;

  const int32_t hQ8 = ((int32_t)(h - 1)) << 8;
  int32_t yLast = 0;

  // Own copy of the series so the optional low pass cannot touch the ring the
  // readings are printed from.
  float series[PC_HISTORY_LEN];
  const int n = (int)buildChartSeries(hist, series, chartSmoothing);

  // The pane colour under the plot depends only on the row, so evaluate it
  // once per row rather than once per pixel. Measured on the S3: leaving this
  // in the inner loop cost 69 ms per Glass Tiles frame (26k calls, each with
  // an integer divide inside glassSkyAt) against a 50 ms budget.
  static const int16_t CHART_H_MAX = 160;
  if (h > CHART_H_MAX) h = CHART_H_MAX;
  Rgb rowBase[CHART_H_MAX];
  for (int16_t yy = 0; yy < h; yy++) {
    rowBase[yy] = glassPaneRow((int16_t)(paneY + paneLocalY + yy),
                               (int16_t)(paneLocalY + yy), paneH, paneAccent, gs);
  }

  // The pane's specular is f(row) * shape(column) and glassPaneRow carries only
  // the body, so a plot that overlaps the gloss band erases the sheen and reads
  // as a flat box stamped on the pane. On a Duo capsule the chart starts 8px
  // below the top and runs nearly its full height, which is where this showed
  // up. Rebuild the row factor here and the column factor per column below.
  // Costs nothing on a surface without gloss: Liquid's glossA is 0, so glossH
  // is 0 and the whole path drops out.
  const int16_t glossH = gs.glossA ? (int16_t)(((int32_t)paneH * gs.glossPct) / 100) : 0;
  uint16_t glossRow[CHART_H_MAX];
  if (glossH > 0) {
    for (int16_t yy = 0; yy < h; yy++) {
      const int16_t dy = (int16_t)(paneLocalY + yy);
      int32_t a = 0;
      if (dy >= 0 && dy < glossH) {
        const int32_t f = ALPHA_FULL_Q8 - ((int32_t)dy * ALPHA_FULL_Q8) / glossH;
        // Same quadratic falloff, and the same overflow dodge, as the pane.
        const int32_t glossF = ((f >> 8) * f) / 255;
        a = ((int32_t)gs.glossA * glossF) / 255;
      }
      glossRow[yy] = (uint16_t)a;
    }
  }
  const int32_t glossCxQ8 = ((int32_t)paneW << 8) / 2;
  // Lifted off the accent so the stroke separates from its own area fill. On a
  // pale pane that lift has to go the other way or the whole plot washes out -
  // it is what made the Frosted light charts read as blank cards.
  const Rgb lineInk = rgbShade(lineAccent, glassLightMode() ? -70 : 40);
  // Area fill alpha, Q8, from the portal's Chart fill slider. The floor keeps
  // the shipped 26:150 ratio to the top so one control still fades the wash out
  // toward the bottom of the plot instead of turning it into a flat block.
  const int32_t AREA_TOP = ((int32_t)glassChartFillNow() * ALPHA_FULL_Q8) / 100;
  const int32_t AREA_BOT = (AREA_TOP * 26) / 150;

  // Column geometry first, so the stroke can span between neighbours. The half
  // stroke is a touch over one pixel: thinner reads as a dotted line once a
  // steep segment spreads it across many rows.
  static const int32_t STROKE_HALF_Q8 = 170;   // ~1.3px stroke
  int32_t yPrev = 0;
  const float denom = (w > 1) ? (float)(w - 1) : 1.0f;
  // Lag the window by one sample and slide it forward across the packet
  // interval: at scroll 0 the window ends at the second-newest reading, at
  // full scroll it ends exactly on the newest - which is the moment the next
  // packet lands. Continuous motion with no seam at the handover.
  const bool glide = (n >= 3);
  const float shift = glide ? ((float)scrollQ8 / 256.0f) : 0.0f;

  // ONE PIXEL IS ALWAYS THE SAME NUMBER OF SAMPLES, whether the ring holds 4
  // readings or 60. Scaling the window to hist.count instead (what this did
  // before) re-fitted the whole series across the full width on every packet
  // while the ring filled, so early on one sample was tens of pixels wide and
  // the sub-sample glide swung the plot back and forth until the ring was full
  // about a minute in. Now the newest sample is pinned to the right edge, the
  // series grows in from the right, and columns older than the data are left as
  // bare pane.
  const float step = (float)(PC_HISTORY_LEN - 2) / denom;
  const float fiRight = (glide ? (float)(n - 2) : (float)(n - 1)) + shift;
  // First column that has data behind it.
  int16_t xStart = (int16_t)(w - 1 - (int32_t)(fiRight / step));
  if (xStart < 0) xStart = 0;
  if (xStart > w) xStart = w;

  // Each column owns the polyline from its midpoint with the PREVIOUS sample to
  // its midpoint with the NEXT one. The union of those spans covers the line
  // with no holes. Spanning only back to the previous midpoint (what this did
  // before) left the far half of every segment undrawn, so a one-column spike
  // came out as a fragment floating above the series with a gap beneath it.
  int32_t yCur = glassChartY(series, n, fiRight - (float)(w - 1 - xStart) * step,
                             lo, span, hQ8);
  yPrev = yCur;

  for (int16_t xi = xStart; xi < w; xi++) {
    const int32_t yNext = (xi + 1 < w)
      ? glassChartY(series, n, fiRight - (float)(w - 2 - xi) * step, lo, span, hQ8)
      : yCur;

    const int32_t midPrev = (yPrev + yCur) / 2;
    const int32_t midNext = (yCur + yNext) / 2;
    int32_t top = yCur, bot = yCur;
    if (midPrev < top) top = midPrev;
    if (midPrev > bot) bot = midPrev;
    if (midNext < top) top = midNext;
    if (midNext > bot) bot = midNext;

    // The area starts under the curve AT THIS COLUMN, not under the connecting
    // segment: keying it off the segment bottom makes the fill bulge sideways
    // out of every spike.
    const int32_t fillTop = yCur + STROKE_HALF_Q8;
    top -= STROKE_HALF_Q8;
    bot += STROKE_HALF_Q8;

    const int16_t px = ox + xi;
    // Leading-edge fade so a chart that starts inside a pane has no seam. It
    // rides xStart, not the plot edge: while the ring is still filling the
    // series begins partway across, and that start is what needs softening.
    const int16_t rel = xi - xStart;
    const int32_t edgeA = (fadeIn > 0 && rel < fadeIn)
      ? ((int32_t)rel * 255) / fadeIn : 255;

    // Column half of the specular, constant all the way down the column.
    int32_t glossShape = 0;
    if (glossH > 0) {
      const int32_t pxC = (((int32_t)(ox + xi)) << 8) + 128;
      const int32_t t = ((pxC - glossCxQ8) * 255) / (glossCxQ8 > 0 ? glossCxQ8 : 1);
      glossShape = 255 - (t * t * gs.glossBow) / (255 * 255);
      if (glossShape < 0) glossShape = 0;
    }

    for (int16_t yy = 0; yy < h; yy++) {
      const int32_t rowT = ((int32_t)yy) << 8, rowB = rowT + 256;

      // Whatever the pane would have been at this pixel is the chart's ground,
      // specular included.
      Rgb base = rowBase[yy];
      if (glossShape > 0 && glossRow[yy]) {
        int32_t gal = ((int32_t)glossRow[yy] * glossShape) / 255;
        if (gal > ALPHA_FULL_Q8) gal = ALPHA_FULL_Q8;
        base = rgbMixQ8(base, RGB_WHITE, gal);
      }

      // Area fill under the stroke, fading with depth. Depth is a Q12 fraction
      // rather than a 0..255 step for the same reason the pane ramps carry a
      // fraction: over a 60-row plot a whole alpha step is a visible contour.
      // Q12 and not Q8 of 255 because a Q8 alpha times a Q8 depth overshoots
      // int32 at full plot height.
      int32_t al = -1;
      if (rowT >= fillTop) {
        const int32_t depth = hQ8 > fillTop
          ? (((rowT - fillTop) << 12) / (hQ8 - fillTop + 1)) : 4096;
        al = AREA_TOP - (((AREA_TOP - AREA_BOT) * depth) >> 12);
      } else if (rowB > fillTop) {
        // Partial row at the fill's top edge.
        al = (AREA_TOP * (rowB - fillTop)) >> 8;
      }
      if (al > 0) {
        al = (al * edgeA) / 255;
        base = rgbMixQ8(base, lineAccent, al > ALPHA_FULL_Q8 ? ALPHA_FULL_Q8 : al);
      }

      // Stroke coverage for this row.
      const int32_t a = rowT > top ? rowT : top;
      const int32_t b = rowB < bot ? rowB : bot;
      int32_t cov = b - a;
      if (cov > 0) {
        // Same uint8_t trap as the duo meter: clamping to 256 wrapped every
        // FULLY covered row to alpha 0, so the solid middle of the stroke was
        // never drawn. Only the antialiased partial rows at the very top and
        // bottom of a segment survived, which is what shredded steep edges into
        // detached specks.
        if (cov > 255) cov = 255;
        cov = (cov * edgeA) / 255;
        if (cov > 0) base = rgbMix(base, lineInk, (uint8_t)cov);
      }

      gcPixel(c, px, oy + yy, rgbTo565(base, px, oy + yy));
    }
    if (xi == w - 1) yLast = yCur;
    yPrev = yCur;
    yCur = yNext;
  }

  // Endpoint marker: a soft round dot rather than the hard filled circle the
  // flat faces use, so the live end of the series reads without a stamped edge.
  {
    const int16_t cxp = ox + w - 3, cyp = oy + (int16_t)(yLast >> 8);
    for (int16_t dy = -2; dy <= 2; dy++) {
      for (int16_t dx = -2; dx <= 2; dx++) {
        const int32_t d2 = dx * dx + dy * dy;
        if (d2 > 5) continue;
        const uint8_t al = (d2 <= 1) ? 255 : (d2 <= 2 ? 190 : 90);
        const int16_t qx = cxp + dx, qy = cyp + dy;
        if (qy < oy || qy >= oy + h || qx < ox || qx >= ox + w) continue;
        const Rgb dot = rgbShade(rowBase[qy - oy],
                                 glassLightMode() ? -(int16_t)al : (int16_t)al);
        gcPixel(c, qx, qy, rgbTo565(dot, qx, qy));
      }
    }
  }
}
