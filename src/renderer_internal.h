#ifndef RENDERER_INTERNAL_H
#define RENDERER_INTERNAL_H

// Shared internals of the monitor renderer, split across four translation
// units: render_primitives (type, layout and chart helpers), glass_surface
// (the compositor), render_faces (the four layouts) and renderer (screens,
// dispatch and the public API).
//
// Only what actually crosses a file boundary lives here. The per-pixel helpers
// deliberately do NOT: every hot loop is compiled in the same unit as the
// helpers it calls, because this project builds without LTO and an inner loop
// calling across units would pay a real call per pixel.

#include "display_ui.h"
#include "display_gauges.h"
#include "pc_metrics.h"
#include "settings.h"
#include "fonts.h"
#include "config.h"
#include "layout.h"
#include "glass_surface.h"

// ---------------------------------------------------------------------------
//  Frame state
// ---------------------------------------------------------------------------
// True while the /screen.bmp handler renders into its capture sprite. The
// capture shares this renderer's code but must not consume the panel's pacing
// state (new-packet flags, layout counts, scale smoothing, previous text
// widths).
extern bool gCaptureRender;
// True when the panel content was actually wiped (screen-state transition,
// face/status flip, online flip). Faces repaint static chrome only then.
extern bool gScreenCleared;
// Debounced companion status, shared with the status badge.
extern uint8_t gStableStatus;

// RAII SPI-transaction bracket, same rationale as display_gauges' ScopedWrite:
// one transaction per face frame so WiFi/UDP servicing never interleaves
// between the primitives (the gauge-flicker chassis invariant).
class RendererWrite {
  lgfx::LovyanGFX& _t;
 public:
  explicit RendererWrite(lgfx::LovyanGFX& t) : _t(t) { _t.startWrite(); }
  ~RendererWrite() { _t.endWrite(); }
};

// ---------------------------------------------------------------------------
//  Colour helpers
// ---------------------------------------------------------------------------
uint16_t blend565(uint8_t alpha, uint16_t fg, uint16_t bg);
uint16_t autoContrast565(uint16_t bg);
uint16_t themedLabelColor(uint16_t accent, uint16_t bg, uint16_t classicColor);
uint16_t themedTileColor(uint16_t accent);

// ---------------------------------------------------------------------------
//  Readings
// ---------------------------------------------------------------------------
struct MetricText {
  char value[12];
  char unit[8];
};
void formatMetricText(const PcMetric& metric, float raw, MetricText& out);

// Bound slots whose metric is present in the live packet, in slot order.
struct VisSlot {
  uint8_t slotIdx;           // index into gaugeMap.slots / pcHistory
  const GaugeSlot* slot;
  const PcMetric* metric;
  const char* label;
};
uint8_t collectVisibleSlots(VisSlot out[NUM_GAUGE_SLOTS]);
float slotScaleMax(const GaugeSlot& slot, const PcMetric& m);
float slotFraction(float value, float scale);
bool  slotWarn(uint8_t slotIdx, float value, float scale);
void  slotProbe(const GaugeSlot& s, const PcMetric& m, char* buf, size_t len);

// Companion hiccup guards: a change must persist across consecutive frames
// before the layout reacts.
bool layoutCountReady(uint8_t n);
uint8_t debouncedStatus();

bool ensureSprite(lgfx::LGFX_Sprite& spr, int16_t& curW, int16_t& curH,
                  int16_t w, int16_t h);
bool largeCanvas(int16_t w, int16_t h);

// ---------------------------------------------------------------------------
//  Font ladders and uniform group sizing
// ---------------------------------------------------------------------------
extern const FontID TEXT_LADDER[];
extern const uint8_t TEXT_LADDER_N;
extern const FontID VALUE_LADDER[];
extern const uint8_t VALUE_LADDER_N;
extern const FontID UNIT_LADDER[];
extern const int16_t VALUE_BOX_SLACK;

uint8_t textLadderIndex(FontID base);
uint8_t fitTextRung(lgfx::LovyanGFX& gfx, const char* s, int16_t maxW, FontID base);
FontID  fitFontForWidth(const char* s, int16_t maxW, FontID base);
uint8_t fitValueRung(lgfx::LovyanGFX& gfx, const char* s, int16_t maxW, int16_t maxH);
FontID  fitValueFont(lgfx::LovyanGFX& gfx, const char* s, int16_t maxW, int16_t maxH);
FontID  upgradeUnitFont(lgfx::LovyanGFX& gfx, const char* unit, int16_t slackW,
                        int16_t valueFh, int16_t smallUnitW);
uint8_t unitLadderIndex(FontID f);
int16_t fontDescent(int16_t fontHeight);
int16_t valueBaseline(int16_t gapY, int16_t band, int16_t fh);
int16_t unitBaselineShift(int16_t valueFh, int16_t unitFh);

// ---------------------------------------------------------------------------
//  Charts and meters
// ---------------------------------------------------------------------------
// Chart motion: the plot glides a fraction of a sample between packets. The
// reading itself is never eased.
extern uint16_t gScrollQ8;
extern bool gNewSample;
void advanceChartMotion();

// Copy the ring oldest->newest, optionally low-passed.
uint8_t buildChartSeries(const SlotHistory& hist, float* out, uint8_t passes);
float histSmooth(const float* s, int n, float fi);
bool sparkBounds(const SlotHistory& hist, uint8_t slotIdx, bool advance,
                 float& lo, float& span);

void drawMeterBar(int16_t x, int16_t y, int16_t w, int16_t h, float frac,
                  uint16_t color);
void sparkPlot(lgfx::LovyanGFX& g, const SlotHistory& hist, uint8_t slotIdx,
               int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color,
               bool advance, uint16_t scrollQ8);
void drawSparkline(const SlotHistory& hist, uint8_t slotIdx, int16_t x, int16_t y,
                   int16_t w, int16_t h, uint16_t color, uint16_t bg,
                   bool advance, uint16_t scrollQ8);
void drawTextScrim(lgfx::LGFX_Sprite& spr, int16_t x, int16_t y, int16_t w,
                   int16_t h, uint16_t bg, uint8_t alpha);

// ---------------------------------------------------------------------------
//  Flicker-free value regions
// ---------------------------------------------------------------------------
void drawValueRegionL(int16_t x, int16_t baseY, int16_t regionW, int16_t bandH,
                      const char* value, const char* unit, uint16_t fg,
                      uint16_t bg, int16_t* prevVw, FontID unitFont);
void drawValueRegionR(int16_t leftBound, int16_t rightX, int16_t cy,
                      int16_t bandH, const char* s, uint16_t fg, uint16_t bg);

// ---------------------------------------------------------------------------
//  Surfaces and faces
// ---------------------------------------------------------------------------
// A layout says how the bound slots are arranged; a surface says how that
// arrangement is painted. The context is deliberately thin - all geometry lives
// in the layout's own face spec, and only a handful of paint helpers branch on
// the surface. Carrying the glass template as a value is what makes a new
// layout x glass combination cheap.
struct SurfaceCtx {
  uint8_t    id;      // SurfaceId
  bool       glass;   // paints a gradient backdrop instead of a flat fill
  GlassStyle tmpl;    // meaningful when glass is set
};
SurfaceCtx surfaceCtxFor(uint8_t surfaceId);

// The face the renderer is currently drawing: the live tuple, snapped to a
// legal pair. Bands and row style come from the user's settings, not from the
// family's seeded defaults, so a customised Duo renders what was asked for.
struct ActiveFace {
  uint8_t layout, surface, bands, rowStyle;
};
ActiveFace activeFace();

// One entry point per layout.
void drawBigNumbersScreen(bool fr);
void drawTilesLayout(bool fr, const SurfaceCtx& sfc);
void drawStripsScreen(bool fr, const SurfaceCtx& sfc);
void drawDuoLayout(bool fr, const SurfaceCtx& sfc, uint8_t configuredBands,
                   uint8_t rowStyle);
// Released when the surface stops being glass.
void glassReleaseSprites();
// The wash hysteresis is stale after a face change or a metric-mapping save.
void resetWashState();

#endif // RENDERER_INTERNAL_H
