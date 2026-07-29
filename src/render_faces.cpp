// The four monitor layouts and the surfaces they wear.
#include "renderer_internal.h"
#include "glass_surface.h"
#include <math.h>

// ---------------------------------------------------------------------------
//  Glass compose buffers
//
//  Two shared sprites serve every window on the panel, so no face has to hold
//  one buffer per card. They are released whenever the surface stops being
//  glass: the glass layouts size their sprites differently and must not both
//  hold one while only one can draw.
// ---------------------------------------------------------------------------
static lgfx::LGFX_Sprite gGlassA, gGlassB;
static int16_t gGlassAW = 0, gGlassAH = 0, gGlassBW = 0, gGlassBH = 0;

void glassReleaseSprites() {
  gGlassA.deleteSprite();
  gGlassB.deleteSprite();
  gGlassAW = gGlassAH = gGlassBW = gGlassBH = 0;
}

// Push the top rows of a compose sprite. The sprite's width IS the window's
// width, so its buffer stride matches and a partial-height blit is just a
// shorter pushImage - which is what lets one allocation serve a card's head
// and its taller body without a resize between them.
static inline void glassBlit(lgfx::LGFX_Sprite& spr, int16_t x, int16_t y,
                             int16_t w, int16_t h) {
  tft.pushImage(x, y, w, h, (const uint16_t*)spr.getBuffer());
  tft.waitDMA();   // shared sprite: barrier before the next window refills it
}

// Subpixel units, so the 8-bit values these were authored as are scaled up.
extern const Rgb GLASS_TINT_AERO   = { 44 * RGB_ONE, 96 * RGB_ONE, 150 * RGB_ONE };
extern const Rgb GLASS_TINT_LIQUID = { 66 * RGB_ONE, 62 * RGB_ONE, 134 * RGB_ONE };

// Value/unit/label onto an already-composed glass window. Transparent text:
// the two-argument setTextColor paints an opaque glyph box, which on glass
// stamps a flat rectangle through the pane. The window is recomposed every
// time it is pushed, so there is no previous text to erase anyway.
//
// The value face is chosen by the caller's group pass, not here, so every card
// on the panel carries the same size.
static void glassHeadText(lgfx::LovyanGFX& g, int16_t x, int16_t cy,
                          int16_t w, const char* label,
                          const MetricText& text, FontID valueFont,
                          FontID unitFont, bool warn, uint16_t accent) {
  setFont(g, FONT_SMALL);
  g.setTextDatum(ML_DATUM);
  g.setTextColor(glassLabelInk(accent));
  g.drawString(label, x + 9, cy);

  g.setTextSize(1.0f);
  setFont(g, valueFont);
  const int16_t valueFh = (int16_t)g.fontHeight();

  // The unit sits on the value's baseline instead of floating at its
  // mid-height: a 10px unit centred against a 59px number reads as detached
  // from it. Both runs use a MIDDLE datum, so shifting the unit's centre by
  // half the box difference lands the two boxes on a common bottom, and taking
  // the descent difference back out lands them on a common BASELINE.
  setFont(g, unitFont);
  const int16_t unitFh = (int16_t)g.fontHeight();
  const int16_t unitW  = (int16_t)g.textWidth(text.unit);
  g.setTextDatum(MR_DATUM);
  g.setTextColor(glassUnitInk());
  g.drawString(text.unit, x + w - 9,
               cy + (valueFh - unitFh) / 2 - unitBaselineShift(valueFh, unitFh));

  setFont(g, valueFont);
  g.setTextDatum(MR_DATUM);
  g.setTextColor(glassValueInk(warn));
  g.drawString(text.value, x + w - 9 - unitW - 4, cy);
  g.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------------------
//  Surface
//
//  A layout says how the bound slots are arranged; a surface says how that
//  arrangement is painted. The context is deliberately thin - all geometry
//  lives in the layout's own face spec, and only the handful of paint helpers
//  below branch on the surface. Carrying the glass template as a value is what
//  makes a new layout x glass combination cheap: the two templates used to be
//  hardcoded at one call site each.
// ---------------------------------------------------------------------------

SurfaceCtx surfaceCtxFor(uint8_t surfaceId) {
  SurfaceCtx c;
  c.id = surfaceId;
  c.glass = surfaceUsesGlass(surfaceId);
  // Built unconditionally: it is a handful of arithmetic on the colourway and
  // keeping it branchless here means every consumer can read ctx.tmpl without
  // first asking whether it is valid.
  c.tmpl = (surfaceId == SURFACE_LIQUID) ? glassLiquid() : glassAero();
  return c;
}

// The sky ramp is a per-frame prerequisite rather than part of painting the
// backdrop: every pane row reads it while composing. The backdrop itself is
// repainted only when something actually blanked the panel.
static void surfaceSkyInit(const SurfaceCtx& sfc, int16_t h) {
  if (!sfc.glass) return;
  // Tint and the three ramp coefficients are what separates the two glass
  // recipes at the whole-panel level.
  if (sfc.id == SURFACE_LIQUID) glassSkyInit(h, GLASS_TINT_LIQUID, 134, 54, 96);
  else                          glassSkyInit(h, GLASS_TINT_AERO,   150, 62, 90);
}

static void surfaceBegin(const SurfaceCtx& sfc, int16_t w, int16_t h) {
  if (sfc.glass) glassBackdrop(w, h);
  else tft.fillRect(0, 0, w, h, dispSettings.bgColor);
}

// ---------------------------------------------------------------------------
//  Load wash
//
//  The Wash surface tints a slot's ground by its reading, quantized to eight
//  steps with hysteresis so a value sitting on a step boundary does not flip
//  the ground back and forth at the packet rate.
//
//  State is keyed by GAUGE SLOT, not by visible cell, so a slot keeps its step
//  when the metric count changes around it. A rebind or a scale change is not
//  visible from here, so the paths that do know reset it - a face change and a
//  metric-mapping save. Without that the hysteresis carries a stale step across
//  a remap and the new metric's first frames wear the old one's intensity.
// ---------------------------------------------------------------------------
static uint8_t gWashQ[NUM_GAUGE_SLOTS];
static uint8_t gWashInit = 0;
static uint16_t gWashGround[NUM_GAUGE_SLOTS];
static uint8_t gWashGroundInit = 0;

void resetWashState() {
  gWashInit = 0;
  gWashGroundInit = 0;
}

// Capture computes the step it would use and commits nothing: a screenshot
// must not advance the live hysteresis.
static uint8_t washStep(uint8_t slotIdx, float frac) {
  const uint8_t bit = (uint8_t)(1u << slotIdx);
  const float qf = frac * 8.0f;
  uint8_t q;
  if ((gWashInit & bit) && fabsf(qf - (float)gWashQ[slotIdx]) < 0.62f) {
    q = gWashQ[slotIdx];
  } else {
    q = (uint8_t)(qf + 0.5f);
    if (q > 8) q = 8;
  }
  if (!gCaptureRender) { gWashQ[slotIdx] = q; gWashInit |= bit; }
  return q;
}

static uint16_t washGround(uint8_t q, uint16_t accent, bool warn, uint16_t bg) {
  const uint8_t alpha = warn ? (uint8_t)(77 + q * 14) : (uint8_t)(26 + q * 13);
  return blend565(alpha, warn ? dispSettings.warnColor : accent, bg);
}

// A changing reading does not necessarily change the quantized wash color.
// Keep the actual rendered ground per gauge slot so the Duo layout can update
// its text without clearing the complete band or cell at packet rate.
static bool washGroundChanged(uint8_t slotIdx, uint16_t ground) {
  const uint8_t bit = (uint8_t)(1u << slotIdx);
  const bool changed = !(gWashGroundInit & bit) || gWashGround[slotIdx] != ground;
  if (!gCaptureRender) {
    gWashGround[slotIdx] = ground;
    gWashGroundInit |= bit;
  }
  return changed;
}

// The face the renderer is currently drawing: the live tuple, snapped to a
// legal pair. Bands and row style come from the user's settings, not from the
// family's seeded defaults, so a customised Duo renders what was asked for.

ActiveFace activeFace() {
  ActiveFace a = { displayLayout, displaySurface, duoHeroBands, duoRowStyle };
  normalizeFace(a.layout, a.surface, a.bands, a.rowStyle);
  return a;
}

static void drawNoMetricsHint(int16_t w, int16_t gridH, bool fr) {
  if (!fr) return;
  tft.setTextDatum(MC_DATUM);
  setFont(tft, FONT_BODY);
  // Transparent on glass: the opaque form stamps a flat box through the
  // gradient. The glass faces paint their backdrop before calling this.
  if (surfaceUsesGlass(activeFace().surface)) tft.setTextColor(glassUnitInk());
  else tft.setTextColor(themeSettings.secondaryColor, dispSettings.bgColor);
  tft.drawString("No metrics bound", w / 2, gridH / 2);
}

// Shared frame preamble for the merged layouts. Collects the visible slots and,
// when their count moved, blanks the panel and forces a full repaint. Capture
// never updates the settled count - a screenshot must not consume the layout
// debounce that protects the panel from transient telemetry changes.
static bool beginFaceFrame(VisSlot* vis, uint8_t& n, uint8_t& lastN,
                           bool& fr, bool& relayout, const SurfaceCtx& sfc) {
  n = collectVisibleSlots(vis);
  relayout = false;
  if (!layoutCountReady(n)) return false;
  if (!gCaptureRender && n != lastN) {
    if (!fr) {
      // Glass repaints its own backdrop below; a flat clear would punch a
      // visible hole through the gradient.
      if (!sfc.glass)
        tft.fillRect(0, 0, (int16_t)tft.width(), (int16_t)tft.height(),
                     dispSettings.bgColor);
      resetGaugeTextCache();
    }
    lastN = n;
    fr = true;
    relayout = true;
  }
  return true;
}

// ---------------------------------------------------------------------------
//  STYLE_BIG_NUMBERS - uppercase label, one large tabular value, hairline
//  meter carrying the same fraction the arc would. Hairline separators
//  instead of cards keep every pixel for type.
// ---------------------------------------------------------------------------
void drawBigNumbersScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;
  const bool big = largeCanvas(w, h);

  VisSlot vis[NUM_GAUGE_SLOTS];
  uint8_t n = collectVisibleSlots(vis);
  if (!layoutCountReady(n)) return;

  // Metric count changed (companion started/stopped or rebind): relayout.
  static uint8_t lastN = 0xFF;
  if (!gCaptureRender && n != lastN) {
    if (!fr) {
      tft.fillRect(0, 0, w, gridH, bg);
      resetGaugeTextCache();
    }
    lastN = n;
    fr = true;
  }

  if (n == 0) {
    drawNoMetricsHint(w, gridH, fr);
    return;
  }

  const uint8_t cols = (n <= 3) ? 1 : 2;
  const uint8_t rows = (n + cols - 1) / cols;
  const int16_t cellW = w / cols;
  const int16_t cellH = gridH / rows;
  int16_t padX = cellW / 14;
  if (padX < 8) padX = 8;
  if (padX > 24) padX = 24;
  const bool roomy = (cellH > 70);

  RendererWrite rw(tft);

  if (fr) {
    for (uint8_t r = 1; r < rows; r++)
      tft.drawFastHLine(8, r * cellH, w - 16, dispSettings.trackColor);
    if (cols == 2)
      tft.drawFastVLine(w / 2, 6, gridH - 12, dispSettings.trackColor);
  }

  // Every cell has the same geometry, so the value band is cell-relative and
  // only the unit width differs between them. Fit them all, keep the smallest
  // rung, render the grid at that one size.
  setFont(tft, FONT_SMALL);
  const int16_t cellGapY = (roomy ? 8 : 4) + (int16_t)tft.fontHeight();
  const int16_t cellBand = cellH - (roomy ? 12 : 7) - 6 - cellGapY;
  uint8_t vrung = 0;
  for (uint8_t i = 0; i < n; i++) {
    MetricText probeText;
    formatMetricText(*vis[i].metric, vis[i].metric->value, probeText);
    setFont(tft, big ? FONT_XLARGE : FONT_SMALL);
    const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
    char pr[12];
    slotProbe(*vis[i].slot, *vis[i].metric, pr, sizeof(pr));
    const uint8_t r = fitValueRung(tft, pr, cellW - 2 * padX - uW - 5, cellBand);
    if (r > vrung) vrung = r;
  }
  // Same for the unit face: sized per cell, a narrow % grows where a wide RPM
  // cannot, and the grid ends up with two unit sizes. The big canvas derives
  // its unit from the (now uniform) value height, so only the small one needs
  // the group pass.
  uint8_t urung = 0;
  for (uint8_t i = 0; i < n && !big; i++) {
    MetricText probeText;
    formatMetricText(*vis[i].metric, vis[i].metric->value, probeText);
    setFont(tft, FONT_SMALL);
    const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
    char pr[12];
    slotProbe(*vis[i].slot, *vis[i].metric, pr, sizeof(pr));
    setFont(tft, VALUE_LADDER[vrung]);
    const int16_t vFh = (int16_t)tft.fontHeight();
    const int16_t pW = (int16_t)tft.textWidth(pr);
    const uint8_t r = unitLadderIndex(upgradeUnitFont(
        tft, probeText.unit, cellW - 2 * padX - uW - 5 - pW, vFh, uW));
    if (r > urung) urung = r;
  }

  for (uint8_t i = 0; i < n; i++) {
    const int16_t x = (i % cols) * cellW;
    const int16_t y = (i / cols) * cellH;
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const float frac = slotFraction(m.value, scale);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s%u%u", text.value, warn ? "!" : "",
             vrung, urung);
    if (!gaugeTextChanged(x + cellW / 2, y + cellH / 2, key, label, fr)) continue;

    // No cell clear: every element overwrites itself opaquely, so only the
    // pixels that actually changed get repainted (no blink at packet rate).
    setFont(tft, FONT_SMALL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(themedLabelColor(s.arcColor, bg, CLR_TEXT_DARK), bg);
    const int16_t labelY = y + (roomy ? 8 : 4);
    tft.drawString(label, x + padX, labelY);
    const int16_t lw = tft.textWidth(label);
    const int16_t lfh = (int16_t)tft.fontHeight();
    tft.fillRect(x + padX + lw, labelY, cellW - 2 * padX - lw, lfh, bg);

    // Value (bottom-left, above the meter) + dim unit after it. The face grows
    // with the cell, so fewer bound metrics = bigger digits.
    // On the big canvas the unit is reserved at its FINAL size before fitting
    // the value: the small canvas can still measure it at FONT_SMALL and
    // upgrade it from leftover slack, but a 48/68px value leaves no slack, so
    // the unit stayed 10px and looked lost beside the digits.
    if (big) setFont(tft, FONT_XLARGE);
    const int16_t unitW = tft.textWidth(text.unit);
    const int16_t gapY = labelY + lfh;
    const int16_t meterTop = y + cellH - (roomy ? 12 : 7);
    const int16_t availW = cellW - 2 * padX - unitW - 5;
    char probe[12];
    slotProbe(s, m, probe, sizeof(probe));

    // Native face, no setTextSize() magnification, then center the glyph box in
    // the space between the label and the meter. A fixed bottom offset left an
    // obvious void under the label once cells got tall. bandH is pinned to
    // (baseY - gapY) so the vacated-pixel clear inside drawValueRegionL covers
    // exactly that void and can never reach up into the label.
    const int16_t band = meterTop - 6 - gapY;
    const FontID vf = VALUE_LADDER[vrung];
    setFont(tft, vf);
    tft.setTextSize(1.0f);
    const int16_t valueFh = (int16_t)tft.fontHeight();
    const int16_t baseY = valueBaseline(gapY, band, valueFh);
    const int16_t bandH = baseY - gapY;
    // Keep the unit a clear step below the value so it stays subordinate.
    const FontID uf = big ? ((valueFh >= 60) ? FONT_XLARGE : FONT_LARGE)
                          : UNIT_LADDER[urung];
    setFont(tft, vf);
    static int16_t prevVw[NUM_GAUGE_SLOTS];
    if (fr && !gCaptureRender) prevVw[vis[i].slotIdx] = -1;
    drawValueRegionL(x + padX, baseY, cellW - 2 * padX, bandH,
                     text.value, text.unit,
                     warn ? dispSettings.warnColor : themeSettings.valueColor, bg,
                     gCaptureRender ? nullptr : &prevVw[vis[i].slotIdx], uf);

    drawMeterBar(x + padX, y + cellH - (roomy ? 12 : 7), cellW - 2 * padX, 3,
                 frac, warn ? dispSettings.warnColor : s.arcColor);
  }
}

// ---------------------------------------------------------------------------
//  LAYOUT_TILES - a grid of cards, one per metric.
//
//  Flat and the glass surfaces share the grid math exactly and then split on
//  nearly every number inside a card. Those numbers are the spec below, not
//  something the surface can carry: a surface says how a shape is painted, and
//  a 22px head floor is not paint.
//
//  Wash keeps the grid and the frame preamble but replaces the cell interior
//  outright - no head/body split, no chart, and a ground whose intensity
//  follows the reading. Hence its own content mode.
// ---------------------------------------------------------------------------
enum ContentMode : uint8_t {
  CONTENT_HEAD_CHART,  // label + value head strip over a history chart
  CONTENT_BLOCK        // label on top, one large value across the whole cell
};

struct TilesSpec {
  uint8_t content;
  int16_t pad;          // outer panel padding
  int16_t gap;          // gap between cards
  int16_t inset;        // cell -> painted block shrink, per side
  int16_t radius;       // card corner radius
  int16_t headFloor;    // minimum head-strip height
  int16_t chartInset;   // chart x inset inside the card
  int16_t chartTrimW;   // total width removed from the card for the chart
  int16_t chartTrimH;   // total height removed from the body for the chart
  int16_t chartMinBody; // body height below which the chart is skipped
  bool    drawsChart;
  bool    nativeValueLadder;  // VALUE_LADDER + fitValueRung, else TEXT_LADDER
  bool    warnStripe;         // left stripe, else the compositor's lit rim
};

static TilesSpec tilesSpecFor(uint8_t surface) {
  TilesSpec s;
  switch (surface) {
    case SURFACE_WASH:
      // The grid is Tiles' grid: raw cells with no outer pad, each shrunk by
      // two pixels a side.
      s.content = CONTENT_BLOCK;
      s.pad = 0; s.gap = 0; s.inset = 2; s.radius = 5;
      s.headFloor = 0;
      s.chartInset = 0; s.chartTrimW = 0; s.chartTrimH = 0; s.chartMinBody = 0;
      s.drawsChart = false;
      s.nativeValueLadder = true;
      s.warnStripe = false;
      break;
    case SURFACE_AERO:
    case SURFACE_LIQUID:
      s.content = CONTENT_HEAD_CHART;
      s.pad = 4; s.gap = 4; s.inset = 0; s.radius = 7;
      s.headFloor = 24;
      s.chartInset = 7; s.chartTrimW = 14; s.chartTrimH = 8;
      // glassChart applies its own minimum on top of this one.
      s.chartMinBody = 11;
      s.drawsChart = true;
      s.nativeValueLadder = true;
      s.warnStripe = false;
      break;
    default:  // SURFACE_FLAT
      s.content = CONTENT_HEAD_CHART;
      s.pad = 4; s.gap = 4; s.inset = 0; s.radius = 6;
      s.headFloor = 22;
      s.chartInset = 8; s.chartTrimW = 16; s.chartTrimH = 10;
      s.chartMinBody = 18;   // an 8px plot after the 10px trim
      s.drawsChart = true;
      s.nativeValueLadder = false;
      s.warnStripe = true;
      break;
  }
  return s;
}

void drawTilesLayout(bool fr, const SurfaceCtx& sfc) {
  const TilesSpec spec = tilesSpecFor(sfc.id);
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;
  const bool big = largeCanvas(w, h);

  surfaceSkyInit(sfc, h);

  VisSlot vis[NUM_GAUGE_SLOTS];
  uint8_t n = 0;
  bool relayout = false;
  static uint8_t lastN = 0xFF;
  if (!beginFaceFrame(vis, n, lastN, fr, relayout, sfc)) return;

  if (n == 0) {
    // Glass has to lay its own ground down first; the flat surfaces were
    // already cleared to bgColor by the preamble.
    if (fr && sfc.glass) surfaceBegin(sfc, w, h);
    drawNoMetricsHint(w, gridH, fr);
    return;
  }

  // Charts animate at the frame rate: the plot glides a fraction of a sample
  // between packets instead of stepping a whole column at once. `advance`
  // means "a new reading landed", which is the only thing the bounds smoothing
  // may react to, and the reading itself is never eased - motion belongs to
  // the chart. Capture never consumes the pacing.
  //
  // A chartless face must not call this at all: it would advance chart state
  // for a face that has no chart.
  if (spec.drawsChart) advanceChartMotion();
  const bool advance = gCaptureRender ? false : gNewSample;
  const uint16_t scroll = gCaptureRender ? 0 : gScrollQ8;

  const uint8_t cols = (n <= 2) ? 1 : 2;
  const uint8_t rows = (n + cols - 1) / cols;
  const int16_t cardW = (w - 2 * spec.pad - (cols - 1) * spec.gap) / cols;
  const int16_t cardH = (gridH - 2 * spec.pad - (rows - 1) * spec.gap) / rows;
  const int16_t blockW = cardW - 2 * spec.inset;
  const int16_t blockH = cardH - 2 * spec.inset;
  int16_t headH = (cardH * 2) / 5;
  if (headH < spec.headFloor) headH = spec.headFloor;
  const int16_t bodyH = cardH - headH;

  RendererWrite rw(tft);

  // Ground is only repainted when something actually blanked the area. A plain
  // full redraw (preview capture, face save) repaints content opaquely, and
  // refilling underneath it would flash the card empty first.
  const bool groundRepaint = fr && (gScreenCleared || relayout || gCaptureRender);
  if (sfc.glass && groundRepaint) surfaceBegin(sfc, w, h);

  // One compose sprite for every glass window on the panel: sized to the taller
  // of the two windows so the head and the body share the allocation.
  const bool off = sfc.glass &&
    ensureSprite(gGlassA, gGlassAW, gGlassAH, cardW, headH > bodyH ? headH : bodyH);

  // Uniform type across the cards - see the font ladder notes. Measured on the
  // panel; the compose sprites render the same glyph metrics.
  FontID valueFont = FONT_BODY;
  FontID unitFont  = FONT_SMALL;
  uint8_t rung = 0, unitRung = 0;

  if (spec.content == CONTENT_BLOCK) {
    setFont(tft, big ? FONT_BODY : FONT_SMALL);
    const int16_t blockLabelBot = 7 + (int16_t)tft.fontHeight();
    const int16_t blockBand = (blockH - 8) - blockLabelBot;
    for (uint8_t i = 0; i < n; i++) {
      MetricText probeText;
      formatMetricText(*vis[i].metric, vis[i].metric->value, probeText);
      setFont(tft, big ? FONT_XLARGE : FONT_SMALL);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[i].slot, *vis[i].metric, pr, sizeof(pr));
      const uint8_t r = fitValueRung(tft, pr, blockW - 18 - uW - 5, blockBand);
      if (r > rung) rung = r;
    }
    valueFont = VALUE_LADDER[rung];
  } else if (spec.nativeValueLadder) {
    // Glass fits the value against the head strip's own height and then picks
    // the unit size independently, so a narrow reading can carry a bigger unit.
    for (uint8_t i = 0; i < n; i++) {
      MetricText probeText;
      formatMetricText(*vis[i].metric, vis[i].metric->value, probeText);
      setFont(tft, FONT_SMALL);
      const int16_t lW = (int16_t)tft.textWidth(vis[i].label);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[i].slot, *vis[i].metric, pr, sizeof(pr));
      const uint8_t r = fitValueRung(tft, pr, cardW - 30 - lW - uW, headH - 6);
      if (r > rung) rung = r;
    }
    valueFont = VALUE_LADDER[rung];
    for (uint8_t i = 0; i < n; i++) {
      MetricText probeText;
      formatMetricText(*vis[i].metric, vis[i].metric->value, probeText);
      setFont(tft, FONT_SMALL);
      const int16_t lW = (int16_t)tft.textWidth(vis[i].label);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[i].slot, *vis[i].metric, pr, sizeof(pr));
      setFont(tft, valueFont);
      const int16_t vFh = (int16_t)tft.fontHeight();
      const int16_t pW = (int16_t)tft.textWidth(pr);
      const uint8_t r = unitLadderIndex(upgradeUnitFont(
          tft, probeText.unit, cardW - 30 - lW - uW - pW, vFh, uW));
      if (r > unitRung) unitRung = r;
    }
    unitFont = UNIT_LADDER[unitRung];
  } else {
    // Flat fits the value on the text ladder against the head's base face.
    const FontID headBase = (headH >= 34) ? FONT_LARGE : FONT_BODY;
    rung = textLadderIndex(headBase);
    for (uint8_t i = 0; i < n; i++) {
      MetricText probeText;
      formatMetricText(*vis[i].metric, vis[i].metric->value, probeText);
      setFont(tft, FONT_SMALL);
      const int16_t lW = (int16_t)tft.textWidth(vis[i].label);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[i].slot, *vis[i].metric, pr, sizeof(pr));
      const uint8_t r = fitTextRung(tft, pr, cardW - 30 - lW - uW, headBase);
      if (r > rung) rung = r;
    }
    valueFont = TEXT_LADDER[rung];
  }

  for (uint8_t i = 0; i < n; i++) {
    const int16_t x = spec.pad + (i % cols) * (cardW + spec.gap);
    const int16_t y = spec.pad + (i / cols) * (cardH + spec.gap);
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const uint8_t slotIdx = vis[i].slotIdx;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(slotIdx, m.value, scale);

    MetricText text;
    formatMetricText(m, m.value, text);

    if (spec.content == CONTENT_BLOCK) {
      const uint8_t q = washStep(slotIdx, slotFraction(m.value, scale));
      const uint16_t cellBg = washGround(q, s.arcColor, warn, bg);
      const uint16_t textC = autoContrast565(cellBg);
      const uint16_t labelC = blend565(200, textC, cellBg);

      char key[16];
      snprintf(key, sizeof(key), "%s%s%u%u", text.value, warn ? "!" : "", q, rung);
      if (!gaugeTextChanged(x + cardW / 2, y + cardH / 2, key, label, fr)) continue;

      static lgfx::LGFX_Sprite blockSpr;
      static int16_t bsW = 0, bsH = 0;
      if (ensureSprite(blockSpr, bsW, bsH, blockW, blockH)) {
        resetFontCache();   // fonts were loaded on the panel, retarget them
        blockSpr.fillSprite(bg);
        blockSpr.fillRoundRect(0, 0, blockW, blockH, spec.radius, cellBg);

        setFont(blockSpr, big ? FONT_BODY : FONT_SMALL);
        blockSpr.setTextDatum(TL_DATUM);
        blockSpr.setTextColor(labelC, cellBg);
        blockSpr.drawString(label, 9, 7);
        const int16_t labelBot = 7 + (int16_t)blockSpr.fontHeight();

        // Native digits instead of a magnified glyph, box centered between the
        // label and the block bottom - the same treatment the other faces get,
        // applied to the sprite this one composes into.
        const int16_t band = (blockH - 8) - labelBot;
        blockSpr.setTextSize(1.0f);
        setFont(blockSpr, valueFont);
        const int16_t valueBaseY =
            valueBaseline(labelBot, band, (int16_t)blockSpr.fontHeight());
        blockSpr.setTextDatum(BL_DATUM);
        blockSpr.setTextColor(textC, cellBg);
        blockSpr.drawString(text.value, 9, valueBaseY);
        const int16_t vw = blockSpr.textWidth(text.value);
        const int16_t valueFh = (int16_t)blockSpr.fontHeight();
        blockSpr.setTextSize(1.0f);
        setFont(blockSpr, big ? ((valueFh >= 60) ? FONT_XLARGE : FONT_LARGE)
                              : FONT_SMALL);
        blockSpr.setTextColor(labelC, cellBg);
        // Baselines, not box bottoms - see drawValueRegionL for why BL_DATUM
        // needs the descent difference taken out.
        blockSpr.drawString(text.unit, 9 + vw + 5,
                            valueBaseY - (fontDescent(valueFh) -
                                          fontDescent((int16_t)blockSpr.fontHeight())));

        blockSpr.pushSprite(tft_ptr, x + spec.inset, y + spec.inset);
        tft.waitDMA();      // shared sprite: barrier before the next block refill
        resetFontCache();   // next panel setFont must reload onto the panel
      } else {
        // Sprite unavailable: direct paint (blinks on wash step change only).
        char probe[12];
        slotProbe(s, m, probe, sizeof(probe));
        tft.fillRoundRect(x + spec.inset, y + spec.inset, blockW, blockH,
                          spec.radius, cellBg);
        setFont(tft, FONT_SMALL);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(labelC, cellBg);
        tft.drawString(label, x + spec.inset + 9, y + spec.inset + 7);
        fitFontForWidth(probe, blockW - 18, FONT_XLARGE);
        tft.setTextDatum(BL_DATUM);
        tft.setTextColor(textC, cellBg);
        tft.drawString(text.value, x + spec.inset + 9, y + spec.inset + blockH - 8);
        const int16_t vw = tft.textWidth(text.value);
        setFont(tft, FONT_SMALL);
        tft.setTextColor(labelC, cellBg);
        tft.drawString(text.unit, x + spec.inset + 9 + vw + 5,
                       y + spec.inset + blockH - 8);
        tft.setTextDatum(TL_DATUM);
      }
      continue;
    }

    // CONTENT_HEAD_CHART from here down.
    // The chart wears the slot's identity color unconditionally: a warn flip
    // must not recolor three minutes of history (stripe or rim and the value
    // carry it).
    const uint16_t accent565 = s.arcColor;
    const uint16_t cardBg = themedTileColor(accent565);
    const uint16_t labelColor = themedLabelColor(accent565, cardBg, CLR_TEXT_DIM);
    const Rgb paneAccent = rgbFrom565(accent565);
    const Rgb warnRgb = rgbFrom565(dispSettings.warnColor);
    const Rgb* warnRim = (warn && !spec.warnStripe) ? &warnRgb : nullptr;

    char key[16];
    if (spec.nativeValueLadder)
      snprintf(key, sizeof(key), "%s%s%u%u", text.value, warn ? "!" : "",
               rung, unitRung);
    else
      snprintf(key, sizeof(key), "%s%s%u", text.value, warn ? "!" : "", rung);
    const bool headChanged = gaugeTextChanged(x + cardW / 2, y, key, label, fr);

    if (sfc.glass) {
      if (headChanged) {
        if (off) {
          resetFontCache();
          GlassCanvas c = glassCanvasFor(gGlassA, &gGlassA, 0, 0);
          glassPaneWindow(c, 0, 0, cardW, headH, y, cardW, cardH, spec.radius,
                          paneAccent, sfc.tmpl, warnRim);
          glassHeadText(gGlassA, 0, headH / 2 + 1, cardW, label,
                        text, valueFont, unitFont, warn, accent565);
          glassBlit(gGlassA, x, y, cardW, headH);
          resetFontCache();
        } else {
          GlassCanvas c = glassCanvasFor(tft, nullptr, x, y);
          glassPaneWindow(c, 0, 0, cardW, headH, y, cardW, cardH, spec.radius,
                          paneAccent, sfc.tmpl, warnRim);
          glassHeadText(tft, x, y + headH / 2 + 1, cardW, label,
                        text, valueFont, unitFont, warn, accent565);
        }
      }

      if (spec.drawsChart && bodyH >= spec.chartMinBody) {
        if (off) {
          // oy = -headH maps pane row headH onto sprite row 0, so the body
          // window is composed at the top of the same buffer the head used.
          GlassCanvas c = glassCanvasFor(gGlassA, &gGlassA, 0, -headH);
          glassPaneWindow(c, 0, headH, cardW, bodyH, y, cardW, cardH, spec.radius,
                          paneAccent, sfc.tmpl, warnRim);
          glassChart(c, pcHistory[slotIdx], slotIdx,
                     spec.chartInset, headH + 2, cardW - spec.chartTrimW,
                     bodyH - spec.chartTrimH,
                     paneAccent, paneAccent, y, headH + 2, cardW, cardH, 0,
                     sfc.tmpl, advance, scroll);
          glassBlit(gGlassA, x, y + headH, cardW, bodyH);
        } else {
          GlassCanvas c = glassCanvasFor(tft, nullptr, x, y);
          glassPaneWindow(c, 0, headH, cardW, bodyH, y, cardW, cardH, spec.radius,
                          paneAccent, sfc.tmpl, warnRim);
          glassChart(c, pcHistory[slotIdx], slotIdx,
                     spec.chartInset, headH + 2, cardW - spec.chartTrimW,
                     bodyH - spec.chartTrimH,
                     paneAccent, paneAccent, y, headH + 2, cardW, cardH, 0,
                     sfc.tmpl, advance, scroll);
        }
      }
      continue;
    }

    if (groundRepaint)
      tft.fillRoundRect(x, y, cardW, cardH, spec.radius, cardBg);

    if (headChanged) {
      // Compose the whole head strip offscreen and push it as ONE blit - the
      // atomic path leaves nothing to blink, exactly like the spark sprite.
      static lgfx::LGFX_Sprite headSpr;
      static int16_t hsW = 0, hsH = 0;
      if (ensureSprite(headSpr, hsW, hsH, cardW, headH)) {
        resetFontCache();   // fonts were loaded on the panel, retarget them
        headSpr.fillSprite(bg);   // panel bg shows through the corner notches
        // Card top with rounded corners; drawn taller than the strip so the
        // bottom edge clips square and joins the card body seamlessly.
        headSpr.fillRoundRect(0, 0, cardW, headH + 8, spec.radius, cardBg);
        if (warn && spec.warnStripe)
          headSpr.fillRect(0, 6, 3, headH - 6, dispSettings.warnColor);

        const int16_t headCy = headH / 2 + 1;
        setFont(headSpr, FONT_SMALL);
        const int16_t unitW = headSpr.textWidth(text.unit);
        headSpr.setTextDatum(ML_DATUM);
        headSpr.setTextColor(labelColor, cardBg);
        headSpr.drawString(label, 9, headCy);
        headSpr.setTextDatum(MR_DATUM);
        headSpr.setTextColor(themeSettings.secondaryColor, cardBg);
        headSpr.drawString(text.unit, cardW - 9, headCy);

        setFont(headSpr, valueFont);
        headSpr.setTextDatum(MR_DATUM);
        headSpr.setTextColor(warn ? dispSettings.warnColor : themeSettings.valueColor,
                             cardBg);
        headSpr.drawString(text.value, cardW - 9 - unitW - 4, headCy);

        headSpr.pushSprite(tft_ptr, x, y);
        tft.waitDMA();      // same DMA barrier as the spark sprite
        resetFontCache();   // next panel setFont must reload onto the panel
      } else {
        // Sprite unavailable: legacy direct path.
        const int16_t headCy = y + headH / 2 + 1;
        setFont(tft, FONT_SMALL);
        const int16_t labelW = tft.textWidth(label);
        const int16_t unitW  = tft.textWidth(text.unit);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(labelColor, cardBg);
        tft.drawString(label, x + 9, headCy);
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(themeSettings.secondaryColor, cardBg);
        tft.drawString(text.unit, x + cardW - 9, headCy);
        setFont(tft, valueFont);
        drawValueRegionR(x + 9 + labelW + 6, x + cardW - 9 - unitW - 4, headCy,
                         headH - 4, text.value,
                         warn ? dispSettings.warnColor : themeSettings.valueColor,
                         cardBg);
      }
      // Warn stripe below the head strip (small overpaint, cannot blink).
      if (spec.warnStripe)
        tft.fillRect(x, y + headH, 3, cardH - headH - 6,
                     warn ? dispSettings.warnColor : cardBg);
    }

    if (spec.drawsChart && bodyH >= spec.chartMinBody) {
      drawSparkline(pcHistory[slotIdx], slotIdx,
                    x + spec.chartInset, y + headH + 2,
                    cardW - spec.chartTrimW, bodyH - spec.chartTrimH,
                    accent565, cardBg, advance, scroll);
    }
  }
}

// ---------------------------------------------------------------------------
//  LAYOUT_STRIPS - one full-width sparkline lane per metric, the label and
//  reading laid over the chart. Each lane composes offscreen and pushes as one
//  blit; because the text sits on the chart, the whole lane repaints at the
//  chart cadence, so readings ride that pace by design.
//
//  Wash tints the WHOLE lane rather than regions inside it: the lanes share one
//  reusable full-width sprite that is refilled before every push, so a per-lane
//  ground is the only granularity the buffer can carry. That one colour is then
//  threaded through the sprite clear, the chart ground, both text scrims and
//  the contrast-derived inks.
// ---------------------------------------------------------------------------
// How far the scrim under the lane text pulls the chart back toward the lane
// ground. High enough that a full-height area fill stops competing with the
// type, low enough that the chart's shape still shows through it.
static const uint8_t STRIPS_SCRIM_ALPHA = 205;

void drawStripsScreen(bool fr, const SurfaceCtx& sfc) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;

  VisSlot vis[NUM_GAUGE_SLOTS];
  uint8_t n = collectVisibleSlots(vis);
  if (!layoutCountReady(n)) return;

  static uint8_t lastN = 0xFF;
  if (!gCaptureRender && n != lastN) {
    if (!fr) {
      tft.fillRect(0, 0, w, gridH, bg);
      resetGaugeTextCache();
    }
    lastN = n;
    fr = true;
  }

  if (n == 0) {
    drawNoMetricsHint(w, gridH, fr);
    return;
  }

  // Charts animate at the frame rate, exactly like the glass faces: the plot
  // glides a fraction of a sample between packets instead of stepping a whole
  // column once per sparkRedrawSec. `advance` still means "a new reading
  // landed", which is the only thing the bounds smoothing may react to, and
  // the reading itself is never eased - motion belongs to the chart.
  advanceChartMotion();
  const bool sparkTick = true;
  const bool advance = gCaptureRender ? false : gNewSample;
  const uint16_t scroll = gCaptureRender ? 0 : gScrollQ8;
  if (!(sparkTick || fr)) return;

  const int16_t rowH = gridH / n;
  RendererWrite rw(tft);

  static lgfx::LGFX_Sprite rowSpr;
  static int16_t rsW = 0, rsH = 0;
  const bool off = ensureSprite(rowSpr, rsW, rsH, w, rowH);

  // Uniform reading type down the lanes - see the font ladder notes. Every
  // lane repaints on every chart tick, so there is no repaint key to carry.
  const FontID laneBase = (rowH >= 34) ? FONT_LARGE : FONT_BODY;
  uint8_t laneRung = textLadderIndex(laneBase);
  for (uint8_t i = 0; i < n; i++) {
    MetricText probeText;
    formatMetricText(*vis[i].metric, vis[i].metric->value, probeText);
    setFont(tft, FONT_SMALL);
    const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
    char pr[12];
    slotProbe(*vis[i].slot, *vis[i].metric, pr, sizeof(pr));
    const uint8_t r = fitTextRung(tft, pr, w / 2 - uW - 12, laneBase);
    if (r > laneRung) laneRung = r;
  }
  const FontID laneValueFont = TEXT_LADDER[laneRung];

  for (uint8_t i = 0; i < n; i++) {
    const int16_t y = i * rowH;
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);
    const uint16_t lineColor = s.arcColor;
    const bool wash = sfc.id == SURFACE_WASH;
    const uint8_t q = wash ? washStep(vis[i].slotIdx,
                                      slotFraction(m.value, scale)) : 0;
    const uint16_t ground = wash ? washGround(q, lineColor, warn, bg) : bg;
    const uint16_t valueInk = wash ? autoContrast565(ground)
                                   : (warn ? dispSettings.warnColor
                                           : themeSettings.valueColor);

    MetricText text;
    formatMetricText(m, m.value, text);

    lgfx::LovyanGFX& g = off ? (lgfx::LovyanGFX&)rowSpr : (lgfx::LovyanGFX&)tft;
    const int16_t oy = off ? 0 : y;

    if (off) {
      resetFontCache();   // fonts were loaded on the panel, retarget them
      rowSpr.fillSprite(ground);
    } else {
      tft.fillRect(0, y, w, rowH, ground);
    }
    sparkPlot(g, pcHistory[vis[i].slotIdx], vis[i].slotIdx,
              0, oy + 2, w, rowH - 4, lineColor, advance, scroll);
    if (i) g.drawFastHLine(0, oy, w, dispSettings.trackColor);

    // Overlay text draws foreground-only: the lane is composed fresh, so
    // there is no previous text to erase. Everything is measured before
    // anything is drawn, so the scrims can be laid down under both runs of
    // type first - drawing text and then shading it would wash the type out
    // along with the chart.
    setFont(g, FONT_SMALL);
    const int16_t labelW = g.textWidth(label);
    const int16_t labelH = (int16_t)g.fontHeight();
    const int16_t unitW = g.textWidth(text.unit);

    setFont(g, laneValueFont);
    const int16_t valueRight = w - 6 - unitW - 5;
    const int16_t valueW = g.textWidth(text.value);
    const int16_t valueH = (int16_t)g.fontHeight();
    const int16_t cy = oy + rowH / 2;

    if (off) {
      // The scrim pulls the chart back toward the LANE ground, not the panel
      // background: on a washed lane the two are different colours and shading
      // toward the panel would stamp a grey box on the tint.
      drawTextScrim(rowSpr, 4, oy + 1, labelW + 5, labelH + 4, ground,
                    STRIPS_SCRIM_ALPHA);
      const int16_t readLeft = valueRight - valueW - 5;
      drawTextScrim(rowSpr, readLeft, cy - valueH / 2 - 2,
                    w - 2 - readLeft, valueH + 4, ground, STRIPS_SCRIM_ALPHA);
    }

    g.setTextDatum(MR_DATUM);
    g.setTextColor(valueInk);
    g.drawString(text.value, valueRight, cy);
    setFont(g, FONT_SMALL);
    g.setTextColor(wash ? blend565(200, valueInk, ground)
                        : themeSettings.secondaryColor);
    g.drawString(text.unit, w - 6, cy);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(wash ? blend565(200, valueInk, ground)
                        : themedLabelColor(lineColor, bg, CLR_TEXT_DIM));
    g.drawString(label, 6, oy + 3);

    if (off) {
      rowSpr.pushSprite(tft_ptr, 0, y);
      tft.waitDMA();      // shared sprite: barrier before the next lane refill
      resetFontCache();   // next panel setFont must reload onto the panel
    }
  }
}

// ---------------------------------------------------------------------------
//  LAYOUT_DUO - hero bands over whatever is left.
//
//  Three legacy faces collapse in here: Hero + list (one band, full-width rows),
//  Duo (two bands, two-column grid) and Liquid glass duo (two capsules over
//  pills). The band count is a setting now rather than the literal 2 it used to
//  be, so activeBands - never the configured value - drives the remainder
//  origin, the row count, the visible-slot offset and every band loop. Feeding
//  the configured count straight in would read a second slot that a one-metric
//  panel does not have and underflow the remainder count to 255.
//
//  The two flat faces were authored separately and their band interiors differ
//  by a couple of pixels in every direction. Those numbers are the spec below
//  rather than something quietly unified, so neither face moves.
// ---------------------------------------------------------------------------
struct DuoSpec {
  int16_t bandPadX;      // label and value left inset
  int16_t bandTrimX;     // width removed from the band's text half
  int16_t bandTopY;      // label top offset inside the band
  int16_t bandBotMargin; // gap between the value box and the band bottom
  int16_t unitGap;       // width reserved between the value and its unit
  int16_t valueBack;     // text half ends at w/2 minus this
  int16_t chartBack;     // chart starts at w/2 minus this
  int16_t chartTop;      // chart top inset inside the band
  int16_t chartRight;    // chart right margin
  int16_t chartTrimH;    // height removed from the band for the chart
  int16_t ruleOffset;    // separator row, relative to the band bottom
  bool    bandBackoff;   // band height shrinks once remainder rows follow
};

static DuoSpec duoSpecFor(uint8_t rowStyle) {
  DuoSpec s;
  if (rowStyle == DUO_ROWS_LIST) {
    // Hero + list. Its band is a fixed share of the panel: the rows below it
    // are thin by construction, so the band never has to back off for them.
    s.bandPadX = 12; s.bandTrimX = 24; s.bandTopY = 8; s.bandBotMargin = 12;
    s.unitGap = 6;
    s.valueBack = 0; s.chartBack = 10; s.chartTop = 10; s.chartRight = 10;
    s.chartTrimH = 20; s.ruleOffset = 0;
    s.bandBackoff = false;
  } else {
    // Duo. Tighter margins: the band value competes with the chart for width,
    // and together with the quarter scale steps this is what lets a four-digit
    // RPM reading render a full step larger.
    s.bandPadX = 10; s.bandTrimX = 14; s.bandTopY = 6; s.bandBotMargin = 8;
    s.unitGap = 5;
    s.valueBack = 2; s.chartBack = 2; s.chartTop = 8; s.chartRight = 8;
    s.chartTrimH = 18; s.ruleOffset = -1;
    s.bandBackoff = true;
  }
  return s;
}

void drawDuoLayout(bool fr, const SurfaceCtx& sfc,
                          uint8_t configuredBands, uint8_t rowStyle) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;
  const bool big = largeCanvas(w, h);

  surfaceSkyInit(sfc, h);

  VisSlot vis[NUM_GAUGE_SLOTS];
  uint8_t n = 0;
  bool relayout = false;
  static uint8_t lastN = 0xFF;
  if (!beginFaceFrame(vis, n, lastN, fr, relayout, sfc)) return;

  if (n == 0) {
    if (fr && sfc.glass) surfaceBegin(sfc, w, h);
    drawNoMetricsHint(w, gridH, fr);
    return;
  }

  // Charts animate at the frame rate: the plot glides a fraction of a sample
  // between packets instead of stepping a whole column at once. `advance`
  // means "a new reading landed", which is the only thing the bounds smoothing
  // may react to; the reading itself is never eased.
  advanceChartMotion();
  const bool advance = gCaptureRender ? false : gNewSample;
  const uint16_t scroll = gCaptureRender ? 0 : gScrollQ8;

  if (configuredBands < 1) configuredBands = 1;
  if (configuredBands > DUO_BANDS_MAX) configuredBands = DUO_BANDS_MAX;
  const uint8_t activeBands = (configuredBands < n) ? configuredBands : n;
  const uint8_t rest = (uint8_t)(n - activeBands);

  RendererWrite rw(tft);

  if (sfc.glass) {
    if (fr && (gScreenCleared || relayout || gCaptureRender)) surfaceBegin(sfc, w, h);

    const int16_t margin = 5, vgap = 4;
    const int16_t paneW = w - 2 * margin;
    // Band height backs off as pill rows are added. At eight metrics two bands
    // at 31% left the three pill rows 22px each, which clipped their labels.
    const uint8_t prowsPlanned = rest ? (uint8_t)((rest + 1) / 2) : 0;
    const uint8_t bandPct = (prowsPlanned == 0)
                          ? ((activeBands == 1) ? (big ? 34 : 42) : (big ? 42 : 48))
                          : (prowsPlanned <= 1) ? (big ? 30 : 34)
                          : (prowsPlanned == 2) ? (big ? 26 : 30)
                                                : (big ? 22 : 26);
    const int16_t bandH = (int16_t)((h * bandPct) / 100);
    const int16_t radius = 16;
    const int16_t halfW = paneW / 2;

    const bool off = ensureSprite(gGlassA, gGlassAW, gGlassAH, halfW, bandH);

    // The capsules are the same shape, so they get the same value AND unit type
    // - see the font ladder notes. Measured on the panel; the compose sprite
    // renders the same glyph metrics.
    setFont(tft, FONT_SMALL);
    const int16_t bandLabelFh = (int16_t)tft.fontHeight();
    const int16_t bandValueBand = bandH - 10 - 9 - bandLabelFh;
    uint8_t bandRung = 0;
    for (uint8_t b = 0; b < activeBands; b++) {
      MetricText probeText;
      formatMetricText(*vis[b].metric, vis[b].metric->value, probeText);
      setFont(tft, FONT_SMALL);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[b].slot, *vis[b].metric, pr, sizeof(pr));
      const uint8_t r = fitValueRung(tft, pr, halfW - 28 - uW, bandValueBand);
      if (r > bandRung) bandRung = r;
    }
    uint8_t bandUnitRung = 0;
    for (uint8_t b = 0; b < activeBands; b++) {
      MetricText probeText;
      formatMetricText(*vis[b].metric, vis[b].metric->value, probeText);
      setFont(tft, FONT_SMALL);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[b].slot, *vis[b].metric, pr, sizeof(pr));
      setFont(tft, VALUE_LADDER[bandRung]);
      const int16_t vFh = (int16_t)tft.fontHeight();
      const int16_t pW = (int16_t)tft.textWidth(pr);
      const uint8_t r = unitLadderIndex(upgradeUnitFont(
          tft, probeText.unit, halfW - 28 - uW - pW, vFh, uW));
      if (r > bandUnitRung) bandUnitRung = r;
    }
    const FontID bandUnitFont = UNIT_LADDER[bandUnitRung];

    for (uint8_t b = 0; b < activeBands; b++) {
      const int16_t y = margin + b * (bandH + vgap);
      const PcMetric& m = *vis[b].metric;
      const GaugeSlot& s = *vis[b].slot;
      const char* label = vis[b].label;
      const float scale = slotScaleMax(s, m);
      const bool warn = slotWarn(vis[b].slotIdx, m.value, scale);
      const uint16_t accent565 = s.arcColor;
      const Rgb paneAccent = rgbFrom565(accent565);
      const Rgb warnRgb = rgbFrom565(dispSettings.warnColor);
      const Rgb* warnRim = warn ? &warnRgb : nullptr;

      MetricText text;
      formatMetricText(m, m.value, text);
      char key[16];
      snprintf(key, sizeof(key), "%s%s%u%u", text.value, warn ? "!" : "",
               bandRung, bandUnitRung);
      const bool textChanged = gaugeTextChanged(3, y + bandH, key, label, fr);

      // Left capsule half: name and reading.
      if (textChanged) {
        lgfx::LovyanGFX& g = off ? (lgfx::LovyanGFX&)gGlassA : (lgfx::LovyanGFX&)tft;
        const int16_t gx = off ? 0 : margin, gy = off ? 0 : y;
        if (off) resetFontCache();
        GlassCanvas c = off ? glassCanvasFor(gGlassA, &gGlassA, 0, 0)
                            : glassCanvasFor(tft, nullptr, margin, y);
        glassPaneWindow(c, 0, 0, halfW, bandH, y, paneW, bandH, radius,
                        paneAccent, sfc.tmpl, warnRim);

        setFont(g, FONT_SMALL);
        g.setTextDatum(TL_DATUM);
        g.setTextColor(glassLabelInk(accent565));
        g.drawString(label, gx + 15, gy + 9);
        const int16_t lfh = (int16_t)g.fontHeight();

        const int16_t gapY = gy + 9 + lfh;
        const int16_t band = (gy + bandH - 10) - gapY;
        g.setTextSize(1.0f);
        setFont(g, VALUE_LADDER[bandRung]);
        const int16_t valueFh = (int16_t)g.fontHeight();
        const int16_t baseY = valueBaseline(gapY, band, valueFh);
        g.setTextDatum(BL_DATUM);
        g.setTextColor(glassValueInk(warn));
        g.drawString(text.value, gx + 14, baseY);
        const int16_t vw = (int16_t)g.textWidth(text.value);

        // The unit rides the value's baseline. Both runs use a BOTTOM datum,
        // which places a box bottom rather than a baseline, so the shared baseY
        // was dropping the small face by the difference in descents - 14px under
        // an 83px value, which is what left the % and RPM hanging below their
        // numbers.
        setFont(g, bandUnitFont);
        g.setTextColor(glassUnitInk());
        g.drawString(text.unit, gx + 14 + vw + 5,
                     baseY - unitBaselineShift(valueFh, (int16_t)g.fontHeight()));
        g.setTextDatum(TL_DATUM);

        if (off) {
          glassBlit(gGlassA, margin, y, halfW, bandH);
          resetFontCache();
        }
      }

      // Right capsule half: the chart, full-bleed inside the rounded shape.
      {
        GlassCanvas c = off ? glassCanvasFor(gGlassA, &gGlassA, -halfW, 0)
                            : glassCanvasFor(tft, nullptr, margin, y);
        glassPaneWindow(c, halfW, 0, paneW - halfW, bandH, y, paneW, bandH,
                        radius, paneAccent, sfc.tmpl, warnRim);
        glassChart(c, pcHistory[vis[b].slotIdx], vis[b].slotIdx,
                   halfW + 4, 8, paneW - halfW - 16, bandH - 18,
                   paneAccent, paneAccent, y, 8, paneW, bandH, 10,
                   sfc.tmpl, advance, scroll);
        if (off) glassBlit(gGlassA, margin + halfW, y, paneW - halfW, bandH);
      }
    }

    if (rest == 0) return;

    // Remaining metrics as pills: name, reading, and a meter on the floor.
    const int16_t y0 = margin + activeBands * (bandH + vgap);
    const uint8_t prows = (uint8_t)((rest + 1) / 2);
    const int16_t hgap = 4;
    const int16_t pillW = (w - 2 * margin - hgap) / 2;
    const int16_t pillH = (h - y0 - margin - (prows - 1) * vgap) / prows;
    if (pillH < 16) return;
    const bool poff = ensureSprite(gGlassB, gGlassBW, gGlassBH, pillW, pillH);

    // Pills trade the floor meter for a real chart whenever their measured
    // height can hold a text row and a readable plot. This is geometry-driven,
    // not board-driven: a sparse 240x240 Duo can have more room per pill than a
    // dense layout on the larger panel.
    // Sprite only. Without one the chart would land on the panel through
    // per-pixel drawPixel, which is thousands of windowed SPI writes per pill.
    const int16_t chartInsetX = big ? 13 : 8;
    const int16_t chartBottom = big ? 13 : 5;
    const int16_t chartMinH = big ? 16 : 14;
    int16_t pillTextH = pillH;
    int16_t pillChartH = 0;
    if (poff) {
      // Large pills keep the original proportional text row. Compact pills use
      // one fixed 22px line, which moves the label and reading just high enough
      // to leave a useful plot below without shrinking their fonts needlessly.
      int16_t th = big ? (int16_t)((pillH * 48) / 100) : 22;
      if (th < 22) th = 22;
      if (th > 52) th = 52;
      const int16_t ch = (int16_t)(pillH - th - chartBottom);
      if (ch >= chartMinH) { pillTextH = th; pillChartH = ch; }
    }
    const bool pillChart = pillChartH > 0;
    // Height the value may grow into. With a chart below, that is the text row
    // and NOT the pill - fitting to the pill would pick a face the row cannot
    // hold and the plot would have digits hanging into it.
    const int16_t pillValueBand = pillChart ? (int16_t)(pillTextH - 6)
                                            : (int16_t)(pillH - 12);

    // Uniform value type across the pills - see the font ladder notes. This is
    // what stops a "24 GB" pill rendering a rung under the "38 %" pill beside it.
    uint8_t pillRung = 0;
    for (uint8_t i = 0; i < rest; i++) {
      const uint8_t vi = (uint8_t)(i + activeBands);
      MetricText probeText;
      formatMetricText(*vis[vi].metric, vis[vi].metric->value, probeText);
      setFont(tft, FONT_SMALL);
      const int16_t lW = (int16_t)tft.textWidth(vis[vi].label);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[vi].slot, *vis[vi].metric, pr, sizeof(pr));
      const uint8_t r = fitValueRung(tft, pr, pillW - 34 - lW - uW, pillValueBand);
      if (r > pillRung) pillRung = r;
    }
    const FontID pillValueFont = VALUE_LADDER[pillRung];
    uint8_t pillUnitRung = 0;
    for (uint8_t i = 0; i < rest; i++) {
      const uint8_t vi = (uint8_t)(i + activeBands);
      MetricText probeText;
      formatMetricText(*vis[vi].metric, vis[vi].metric->value, probeText);
      setFont(tft, FONT_SMALL);
      const int16_t lW = (int16_t)tft.textWidth(vis[vi].label);
      const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
      char pr[12];
      slotProbe(*vis[vi].slot, *vis[vi].metric, pr, sizeof(pr));
      setFont(tft, pillValueFont);
      const int16_t vFh = (int16_t)tft.fontHeight();
      const int16_t pW = (int16_t)tft.textWidth(pr);
      const uint8_t r = unitLadderIndex(upgradeUnitFont(
          tft, probeText.unit, pillW - 34 - lW - uW - pW, vFh, uW));
      if (r > pillUnitRung) pillUnitRung = r;
    }
    const FontID pillUnitFont = UNIT_LADDER[pillUnitRung];

    for (uint8_t i = 0; i < rest; i++) {
      const uint8_t vi = (uint8_t)(i + activeBands);
      const int16_t x = margin + (i % 2) * (pillW + hgap);
      const int16_t y = y0 + (i / 2) * (pillH + vgap);
      const PcMetric& m = *vis[vi].metric;
      const GaugeSlot& s = *vis[vi].slot;
      const char* label = vis[vi].label;
      const float scale = slotScaleMax(s, m);
      const bool warn = slotWarn(vis[vi].slotIdx, m.value, scale);
      const uint16_t accent565 = s.arcColor;
      const Rgb paneAccent = rgbFrom565(accent565);
      const Rgb warnRgb = rgbFrom565(dispSettings.warnColor);
      const Rgb* warnRim = warn ? &warnRgb : nullptr;

      MetricText text;
      formatMetricText(m, m.value, text);
      char key[16];
      snprintf(key, sizeof(key), "%s%s%u%u", text.value, warn ? "!" : "",
               pillRung, pillUnitRung);
      // The text half still repaints only when the reading changes; the plot has
      // to keep up with the scroll, so a charted pill stays in the loop either
      // way and takes the cheaper of the two paths below.
      const bool pillTextChanged = gaugeTextChanged(x + 1, y + 1, key, label, fr);
      if (!pillTextChanged && !pillChart) continue;

      if (!pillTextChanged) {
        // Plot band only: everything outside it - rim, corners, the text row -
        // is already on the panel from the last full pass and does not move.
        // The band is composed at full pill width because glassBlit pushes the
        // sprite buffer with the sprite's own stride, so a narrower window would
        // read the rows skewed.
        GlassCanvas cb = glassCanvasFor(gGlassB, &gGlassB, 0, -pillTextH);
        // The plot repaints every pixel of its own rect from the pane row it
        // sits on, so laying the pane down under it first writes those pixels
        // twice - two thirds of the band, every frame. Compose only the two side
        // margins the plot does not cover.
        // NOT while the ring is still filling: the series then starts partway
        // across and glassChart leaves the columns to its left untouched, which
        // on a shared sprite is whatever the previous pill wrote there. The ring
        // is full about a minute after boot and this is the steady state.
        const SlotHistory& phist = pcHistory[vis[vi].slotIdx];
        if (phist.count >= PC_HISTORY_LEN) {
          glassPaneWindow(cb, 0, pillTextH, chartInsetX, pillChartH,
                          y, pillW, pillH, 12, paneAccent, sfc.tmpl, warnRim);
          glassPaneWindow(cb, pillW - chartInsetX, pillTextH,
                          chartInsetX, pillChartH, y, pillW, pillH, 12,
                          paneAccent, sfc.tmpl, warnRim);
        } else {
          glassPaneWindow(cb, 0, pillTextH, pillW, pillChartH, y, pillW, pillH, 12,
                          paneAccent, sfc.tmpl, warnRim);
        }
        glassChart(cb, phist, vis[vi].slotIdx,
                   chartInsetX, pillTextH, pillW - 2 * chartInsetX, pillChartH,
                   paneAccent, paneAccent, y, pillTextH, pillW, pillH, 8,
                   sfc.tmpl, advance, scroll);
        glassBlit(gGlassB, x, y + pillTextH, pillW, pillChartH);
        continue;
      }

      lgfx::LovyanGFX& g = poff ? (lgfx::LovyanGFX&)gGlassB : (lgfx::LovyanGFX&)tft;
      const int16_t gx = poff ? 0 : x, gy = poff ? 0 : y;
      if (poff) resetFontCache();
      GlassCanvas c = poff ? glassCanvasFor(gGlassB, &gGlassB, 0, 0)
                           : glassCanvasFor(tft, nullptr, x, y);
      glassPaneWindow(c, 0, 0, pillW, pillH, y, pillW, pillH, 12,
                      paneAccent, sfc.tmpl, warnRim);

      // Name and reading share one centreline. Stacking them needs ~34px and a
      // pill is often half that, which is what clipped the labels off the
      // bottom row; the meter then takes the floor on its own.
      // With a chart the pair rides the top row instead of the pill's middle,
      // which is the room the plot needs underneath.
      const int16_t cy = pillChart ? (int16_t)(gy + pillTextH / 2)
                                   : (int16_t)(gy + pillH / 2 - 2);
      setFont(g, FONT_SMALL);
      g.setTextDatum(ML_DATUM);
      g.setTextColor(glassLabelInk(accent565));
      g.drawString(label, gx + 12, cy);

      g.setTextSize(1.0f);
      setFont(g, pillValueFont);
      const int16_t valueFh = (int16_t)g.fontHeight();

      // Unit sits on the value's baseline - see glassHeadText for why a middle
      // datum needs both corrections.
      setFont(g, pillUnitFont);
      const int16_t unitFh = (int16_t)g.fontHeight();
      const int16_t unitW = (int16_t)g.textWidth(text.unit);
      g.setTextDatum(MR_DATUM);
      g.setTextColor(glassUnitInk());
      g.drawString(text.unit, gx + pillW - 11,
                   cy + (valueFh - unitFh) / 2 - unitBaselineShift(valueFh, unitFh));

      setFont(g, pillValueFont);
      g.setTextDatum(MR_DATUM);
      g.setTextColor(glassValueInk(warn));
      g.drawString(text.value, gx + pillW - 11 - unitW - 4, cy);
      g.setTextDatum(TL_DATUM);

      // Plot on the capsule floor, composed into the same sprite as the text so
      // the pill still reaches the panel in one push.
      if (pillChart) {
        glassChart(c, pcHistory[vis[vi].slotIdx], vis[vi].slotIdx,
                   chartInsetX, pillTextH, pillW - 2 * chartInsetX, pillChartH,
                   paneAccent, paneAccent, y, pillTextH, pillW, pillH, 8,
                   sfc.tmpl, advance, scroll);
      }

      // Meter on the capsule floor, drawn through the compositor so its ends
      // stay soft against the pane instead of clipping to a hard rectangle.
      const int16_t mx = 13, mw = pillW - 26, my = pillH - 6;
      if (!pillChart && mw > 12 && my > 0) {
        const float frac = slotFraction(m.value, scale);
        const int32_t fillQ8 = (int32_t)(frac * (float)(mw << 8));
        const Rgb fillC = rgbFrom565(warn ? dispSettings.warnColor : accent565);
        for (int16_t dx = 0; dx < mw; dx++) {
          const int32_t pxL = (int32_t)dx << 8;
          int32_t cov = fillQ8 - pxL;
          if (cov < 0) cov = 0;
          // 255, NOT 256: the mix alpha is a uint8_t, so a full-coverage 256
          // truncated to 0 and every solid pixel of the bar drew as pure track.
          // Only the one partially covered pixel at the fill boundary survived,
          // which is why the meter read as a lone tick instead of a bar.
          if (cov > 255) cov = 255;
          for (int16_t dy = 0; dy < 2; dy++) {
            const int16_t qx = gx + mx + dx, qy = gy + my + dy;
            Rgb base = glassPaneRow(y + my + dy, (int16_t)(my + dy), pillH,
                                    paneAccent, sfc.tmpl);
            base = rgbMix(base, RGB_BLACK, 70);                 // track
            if (cov > 0) base = rgbMix(base, fillC, (uint8_t)cov);
            if (poff) gGlassB.drawPixel(qx, qy, rgbTo565(base, qx, qy));
            else tft.drawPixel(qx, qy, rgbTo565(base, qx, qy));
          }
        }
      }

      if (poff) {
        glassBlit(gGlassB, x, y, pillW, pillH);
        resetFontCache();
      }
    }
    return;
  }

  // ---- Flat and washed surfaces --------------------------------------------
  const DuoSpec spec = duoSpecFor(rowStyle);
  // Wash tints each band and each remainder slot independently from its own
  // reading. A washed ground under a history chart is redundant but not
  // contradictory: one carries the present level, the other the trajectory.
  const bool wash = sfc.id == SURFACE_WASH;

  // Two bands at 32% each take 64% of the panel. That is fine at 240px, but on
  // a 480px panel it leaves the meter grid cramped against the bottom edge
  // while the bands themselves hold mostly empty space.
  const int16_t bandH =
      (rest == 0 || !spec.bandBackoff)
        ? ((activeBands == 1) ? (big ? (gridH * 30) / 100 : (gridH * 2) / 5)
                              : (big ? (gridH * 40) / 100 : gridH / 2))
        : (big ? (gridH * 25) / 100 : (gridH * 32) / 100);
  const int16_t chartX = w / 2 - spec.chartBack;
  // The text half gives way to the chart as soon as there is a second metric to
  // plot beside it; on its own the band spans the panel and the chart drops
  // underneath.
  const int16_t valueW = (n == 1) ? w : (int16_t)(w / 2 - spec.valueBack);

  // The bands are the same shape, so they share one value type - see the font
  // ladder notes.
  setFont(tft, FONT_SMALL);
  const int16_t bandGapY = spec.bandTopY + (int16_t)tft.fontHeight();
  const int16_t bandValueBand = bandH - spec.bandBotMargin - bandGapY;
  uint8_t bandRung = 0;
  for (uint8_t b = 0; b < activeBands; b++) {
    MetricText probeText;
    formatMetricText(*vis[b].metric, vis[b].metric->value, probeText);
    setFont(tft, big ? FONT_XLARGE : FONT_SMALL);
    const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
    char pr[12];
    slotProbe(*vis[b].slot, *vis[b].metric, pr, sizeof(pr));
    const uint8_t r = fitValueRung(tft, pr,
                                   valueW - spec.bandTrimX - uW - spec.unitGap,
                                   bandValueBand);
    if (r > bandRung) bandRung = r;
  }
  // One unit face for every band - see the big-numbers cells for why. On the
  // large canvas the unit follows the value height instead.
  uint8_t bandUnitRung = 0;
  for (uint8_t b = 0; b < activeBands && !big; b++) {
    MetricText probeText;
    formatMetricText(*vis[b].metric, vis[b].metric->value, probeText);
    setFont(tft, FONT_SMALL);
    const int16_t uW = (int16_t)tft.textWidth(probeText.unit);
    char pr[12];
    slotProbe(*vis[b].slot, *vis[b].metric, pr, sizeof(pr));
    setFont(tft, VALUE_LADDER[bandRung]);
    const int16_t vFh = (int16_t)tft.fontHeight();
    const int16_t pW = (int16_t)tft.textWidth(pr);
    const uint8_t r = unitLadderIndex(upgradeUnitFont(
        tft, probeText.unit,
        valueW - spec.bandTrimX - uW - spec.unitGap - pW, vFh, uW));
    if (r > bandUnitRung) bandUnitRung = r;
  }

  for (uint8_t b = 0; b < activeBands; b++) {
    const int16_t y = b * bandH;
    const PcMetric& m = *vis[b].metric;
    const GaugeSlot& s = *vis[b].slot;
    const char* label = vis[b].label;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(vis[b].slotIdx, m.value, scale);
    const uint8_t q = wash ? washStep(vis[b].slotIdx,
                                      slotFraction(m.value, scale)) : 0;
    const uint16_t ground = wash ? washGround(q, s.arcColor, warn, bg) : bg;
    const bool groundChanged = wash && washGroundChanged(vis[b].slotIdx, ground);
    const uint16_t valueInk = wash ? autoContrast565(ground)
                                   : (warn ? dispSettings.warnColor
                                           : themeSettings.valueColor);
    const uint16_t labelInk = wash ? blend565(200, valueInk, ground)
                                   : themedLabelColor(s.arcColor, bg, CLR_TEXT_DARK);

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    // The wash step joins the key: a ground change has to repaint the text
    // half, which is what lays the new tint down under it.
    snprintf(key, sizeof(key), "%s%s%u%u%u", text.value, warn ? "!" : "",
             bandRung, bandUnitRung, q);
    // Anchor off-grid (3, band bottom) so it can never collide with a row.
    if (gaugeTextChanged(3, y + bandH, key, label, fr)) {
      // The whole band wears one tint, chart half included. With a single
      // metric the chart drops below the band, so the tint follows it down.
      if (wash && (gScreenCleared || relayout || groundChanged))
        tft.fillRect(0, y, w, (n == 1) ? (int16_t)(gridH - y) : bandH, ground);
      setFont(tft, FONT_SMALL);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(labelInk, ground);
      tft.drawString(label, spec.bandPadX, y + spec.bandTopY);
      const int16_t lw = tft.textWidth(label);
      const int16_t lfh = (int16_t)tft.fontHeight();
      // Strip between the label and the value band: nothing legitimate renders
      // there, so a background repaint is invisible and evicts residue left by
      // an earlier layout or screen.
      tft.fillRect(spec.bandPadX + lw, y + spec.bandTopY,
                   valueW - spec.bandTrimX - lw, lfh, ground);
      const int16_t gapY = y + spec.bandTopY + lfh;

      // Native face, box centered under the label. See the big-numbers cells
      // for why bandV is pinned to (baseY - gapY).
      const int16_t band = (y + bandH - spec.bandBotMargin) - gapY;
      const FontID vf = VALUE_LADDER[bandRung];
      setFont(tft, vf);
      tft.setTextSize(1.0f);
      const int16_t valueFh = (int16_t)tft.fontHeight();
      const int16_t baseY = valueBaseline(gapY, band, valueFh);
      const int16_t bandV = baseY - gapY;
      const FontID uf = big ? ((valueFh >= 60) ? FONT_XLARGE : FONT_LARGE)
                            : UNIT_LADDER[bandUnitRung];
      setFont(tft, vf);
      static int16_t bandPrevVw[DUO_BANDS_MAX] = { -1, -1 };
      if (fr && !gCaptureRender) bandPrevVw[b] = -1;
      drawValueRegionL(spec.bandPadX, baseY, valueW - spec.bandTrimX, bandV,
                       text.value, text.unit, valueInk, ground,
                       gCaptureRender ? nullptr : &bandPrevVw[b], uf);
    }

    if (fr)
      tft.drawFastHLine(8, y + bandH + spec.ruleOffset, w - 16,
                        dispSettings.trackColor);

    if (n >= 2) {
      // Extends slightly into the centre gap while preserving a clear margin
      // after a three-digit band value and its unit.
      drawSparkline(pcHistory[vis[b].slotIdx], vis[b].slotIdx,
                    chartX, y + spec.chartTop, w - chartX - spec.chartRight,
                    bandH - spec.chartTrimH, s.arcColor, ground, advance, scroll);
    } else {
      drawSparkline(pcHistory[vis[b].slotIdx], vis[b].slotIdx,
                    12, bandH + 8, w - 24, gridH - bandH - 16,
                    s.arcColor, ground, advance, scroll);
    }
  }

  if (rest == 0) return;

  const int16_t y0 = activeBands * bandH + 2;

  if (rowStyle == DUO_ROWS_LIST) {
    // Full-width rows: name, thin bar, reading. The bar begins closer to the
    // labels and reaches farther right, leaving a stable value area at the edge.
    const int16_t rowH = (gridH - y0) / rest;
    // Label column. w/4 leaves only 64px of text room at 320px wide, which a
    // 4-character label like "CPUW" overflows at FONT_LARGE - fitFontForWidth
    // then drops it to FONT_BODY, and because the ladder mixes weights
    // (inter_19 is Inter-Bold, inter_14 is Inter-Regular) the label visibly
    // loses its bold as well as its size, so one row looks wrong next to the
    // others. Widen the column on the big canvas so the common labels never
    // need to step down.
    const int16_t bx = big ? (w * 30) / 100 : w / 4;
    const int16_t barRight = (w * 62) / 100;
    const int16_t bw = barRight - bx;
    const int16_t valueLeft = barRight + 3;
    const int16_t valueRight = w - 4;
    const int16_t rowValueW = valueRight - valueLeft;
    int16_t bh = rowH / 5;
    if (bh < 4) bh = 4;
    if (bh > 10) bh = 10;
    // The large canvas gives these rows ~57px each, where the original ceilings
    // (BODY label / LARGE value) read small next to the hero band. Step both up
    // one rung there; the 240x240 boards keep their original ladders. These
    // strings carry the unit ("52 C"), so they must stay on full-charset faces -
    // FONT_NUM_* is digits-only and would drop the letters.
    const FontID rowLabelFont = big ? ((rowH >= 40) ? FONT_LARGE : FONT_BODY)
                                    : ((rowH >= 24) ? FONT_BODY : FONT_SMALL);
    // Uniform reading type down the rows - see the font ladder notes. The rows
    // share one value column, so only the string differs between them.
    const FontID rowValueBase = big ? ((rowH >= 44) ? FONT_XLARGE : FONT_LARGE)
                                    : ((rowH >= 34) ? FONT_LARGE : FONT_BODY);
    uint8_t rowRung = textLadderIndex(rowValueBase);
    for (uint8_t i = 0; i < rest; i++) {
      const uint8_t vi = (uint8_t)(i + activeBands);
      MetricText probeText;
      formatMetricText(*vis[vi].metric,
                       slotScaleMax(*vis[vi].slot, *vis[vi].metric), probeText);
      char pr[20];
      snprintf(pr, sizeof(pr), "%s %s", probeText.value, probeText.unit);
      const uint8_t r = fitTextRung(tft, pr, rowValueW, rowValueBase);
      if (r > rowRung) rowRung = r;
    }
    const FontID rowValueFont = TEXT_LADDER[rowRung];
    for (uint8_t i = 0; i < rest; i++) {
      const uint8_t vi = (uint8_t)(i + activeBands);
      const PcMetric& m = *vis[vi].metric;
      const GaugeSlot& s = *vis[vi].slot;
      const char* label = vis[vi].label;
      const float scale = slotScaleMax(s, m);
      const float frac = slotFraction(m.value, scale);
      const bool warn = slotWarn(vis[vi].slotIdx, m.value, scale);
      const int16_t rowY = y0 + i * rowH;
      const int16_t cy = rowY + rowH / 2;
      const uint8_t q = wash ? washStep(vis[vi].slotIdx, frac) : 0;
      const uint16_t ground = wash ? washGround(q, s.arcColor, warn, bg) : bg;
      const bool groundChanged = wash && washGroundChanged(vis[vi].slotIdx, ground);
      const uint16_t valueInk = wash ? autoContrast565(ground)
                                     : (warn ? dispSettings.warnColor
                                             : themeSettings.valueColor);

      MetricText rowText;
      formatMetricText(m, m.value, rowText);
      char rkey[16];
      snprintf(rkey, sizeof(rkey), "%s%s%u%u", rowText.value, warn ? "!" : "",
               rowRung, q);
      if (!gaugeTextChanged(w / 2, cy, rkey, label, fr)) continue;

      if (wash && (gScreenCleared || relayout || groundChanged))
        tft.fillRect(0, rowY, w, rowH, ground);
      fitFontForWidth(label, bx - 16, rowLabelFont);
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(wash ? blend565(200, valueInk, ground)
                            : themedLabelColor(s.arcColor, bg, CLR_TEXT_DIM), ground);
      tft.drawString(label, 8, cy);

      drawMeterBar(bx, cy - bh / 2, bw, bh, frac,
                   warn ? dispSettings.warnColor : s.arcColor);

      char vb[20];
      snprintf(vb, sizeof(vb), "%s %s", rowText.value, rowText.unit);
      setFont(tft, rowValueFont);
      drawValueRegionR(valueLeft, valueRight, cy, rowH - 2, vb, valueInk, ground);
    }
    return;
  }

  // Two-column grid for the remaining metrics: label left, value right, meter
  // beneath.
  const uint8_t rows = (uint8_t)((rest + 1) / 2);
  const int16_t cellH = (gridH - y0) / rows;
  const int16_t cellW = w / 2;
  // LARGE, not XLARGE: these cells are only ~160px wide and the value shares
  // the row with the label, so an XLARGE 4-digit value plus unit ("462 MB")
  // butts straight into a 4-character label. Still a clear step up from the
  // original BODY ceiling. One rung for the whole grid - see the ladder notes.
  const FontID gridBase = big ? FONT_LARGE : FONT_BODY;
  uint8_t gridRung = textLadderIndex(gridBase);
  for (uint8_t i = 0; i < rest; i++) {
    const uint8_t vi = (uint8_t)(i + activeBands);
    MetricText probeText;
    formatMetricText(*vis[vi].metric,
                     slotScaleMax(*vis[vi].slot, *vis[vi].metric), probeText);
    setFont(tft, big ? FONT_BODY : FONT_SMALL);
    const int16_t lW = (int16_t)tft.textWidth(vis[vi].label);
    char pr[20];
    snprintf(pr, sizeof(pr), "%s %s", probeText.value, probeText.unit);
    const uint8_t r = fitTextRung(tft, pr, cellW - 26 - lW, gridBase);
    if (r > gridRung) gridRung = r;
  }
  const FontID gridValueFont = TEXT_LADDER[gridRung];
  for (uint8_t i = 0; i < rest; i++) {
    const uint8_t vi = (uint8_t)(i + activeBands);
    const PcMetric& m = *vis[vi].metric;
    const GaugeSlot& s = *vis[vi].slot;
    const char* label = vis[vi].label;
    const float scale = slotScaleMax(s, m);
    const float frac = slotFraction(m.value, scale);
    const bool warn = slotWarn(vis[vi].slotIdx, m.value, scale);
    const int16_t x = (i % 2) * cellW;
    const int16_t y = y0 + (i / 2) * cellH;
    const int16_t cy = y + (cellH - 10) / 2;
    const uint8_t q = wash ? washStep(vis[vi].slotIdx, frac) : 0;
    const uint16_t ground = wash ? washGround(q, s.arcColor, warn, bg) : bg;
    const bool groundChanged = wash && washGroundChanged(vis[vi].slotIdx, ground);
    const uint16_t valueInk = wash ? autoContrast565(ground)
                                   : (warn ? dispSettings.warnColor
                                           : themeSettings.valueColor);

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s%u%u", text.value, warn ? "!" : "",
             gridRung, q);
    if (!gaugeTextChanged(x + cellW / 2, cy, key, label, fr)) continue;

    if (wash && (gScreenCleared || relayout || groundChanged))
      tft.fillRect(x, y, cellW, cellH, ground);
    // Same reasoning as the list rows: a 160px-wide cell on the large canvas
    // dwarfed the SMALL label / BODY value ceiling. Unit is in the string, so
    // full-charset faces only.
    setFont(tft, big ? FONT_BODY : FONT_SMALL);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(wash ? blend565(200, valueInk, ground)
                          : themedLabelColor(s.arcColor, bg, CLR_TEXT_DIM), ground);
    tft.drawString(label, x + 10, cy);
    const int16_t labelW = tft.textWidth(label);

    char vb[20];
    snprintf(vb, sizeof(vb), "%s %s", text.value, text.unit);
    setFont(tft, gridValueFont);
    drawValueRegionR(x + 10 + labelW + 6, x + cellW - 10, cy, cellH - 14, vb,
                     valueInk, ground);

    drawMeterBar(x + 10, y + cellH - 9, cellW - 20, 3, frac,
                 warn ? dispSettings.warnColor : s.arcColor);
  }
}
