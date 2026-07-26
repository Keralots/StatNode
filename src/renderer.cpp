// Renderer: screen-state machine + per-frame drawing for StatNode.
// Replaces BambuHelper's printer-centric display_ui rendering. Screens:
//   SPLASH/AP/CONNECTING/CONNECTED - status text
//   MONITOR  - gauge grid driven by pcData (auto-mapped for now; configurable
//              metric->gauge assignment lands in the web-portal step)
//   CLOCK    - idle screen shown when the PC is offline
//   OTA_UPDATE - "Updating..." hold screen
#include "display_ui.h"
#include "display_gauges.h"
#include "clock_mode.h"
#include "clock_mario.h"
#include "clock_pong.h"
#include "pc_metrics.h"
#include "settings.h"
#include "fonts.h"
#include "config.h"
#include "layout.h"
#include "wifi_manager.h"
#include <WiFi.h>
#include <math.h>

static ScreenState currentScreen = SCREEN_SPLASH;
static bool forceRedraw = true;
static bool clearBeforeRedraw = false;
static unsigned long lastUpdate = 0;
static bool prevOnline = false;

ScreenState getScreenState() { return currentScreen; }

void markScreenCleared();  // defined below with the style helpers

void setScreenState(ScreenState state) {
  if (state == currentScreen) return;
  currentScreen = state;
  forceRedraw = true;
  clearBeforeRedraw = false;
  lastUpdate = 0;            // bypass throttle so the new screen paints at once
  tft.fillScreen(dispSettings.bgColor);
  resetGaugeTextCache();
  resetClock();
  resetMarioClock();
  resetPongClock();
  markScreenCleared();
  markFrameDirty();
}

// ---------------------------------------------------------------------------
//  Status screens
// ---------------------------------------------------------------------------
static void drawCenteredLines(const char* title, uint16_t titleColor,
                              const char* l1, const char* l2, const char* l3) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  tft.setTextDatum(MC_DATUM);

  setFont(tft, FONT_LARGE);
  tft.setTextColor(titleColor, dispSettings.bgColor);
  tft.drawString(title, w / 2, h / 2 - 40);

  setFont(tft, FONT_BODY);
  tft.setTextColor(CLR_TEXT, dispSettings.bgColor);
  if (l1 && l1[0]) tft.drawString(l1, w / 2, h / 2 - 6);
  setFont(tft, FONT_SMALL);
  tft.setTextColor(CLR_TEXT_DIM, dispSettings.bgColor);
  if (l2 && l2[0]) tft.drawString(l2, w / 2, h / 2 + 18);
  if (l3 && l3[0]) tft.drawString(l3, w / 2, h / 2 + 36);
}

static void drawApScreen() {
  String ssid = getAPSSID();
  drawCenteredLines("WiFi Setup", CLR_ORANGE,
                    ssid.c_str(), "Pass: " WIFI_AP_PASSWORD, "Open 192.168.4.1");
}

static void drawConnectingScreen() {
  drawCenteredLines("Connecting", CLR_BLUE, wifiSSID, "Please wait...", "");
}

static void drawConnectedScreen() {
  String ip = WiFi.localIP().toString();
  drawCenteredLines("Connected", CLR_GREEN, ip.c_str(), "Open IP in browser", "");
}

static void drawOtaScreen() {
  drawCenteredLines("Updating", CLR_GOLD, "Firmware upload", "Do not power off", "");
}

// ---------------------------------------------------------------------------
//  Monitor screen - configurable metric->gauge mapping.
//  Each of the NUM_GAUGE_SLOTS slots binds a PC metric (by id) to a gauge style,
//  full-scale range, and color (gaugeMap, persisted in NVS, edited from the web
//  portal). Slots are laid out in a responsive 3-column grid sized to the panel.
// ---------------------------------------------------------------------------
static uint8_t classifyByUnit(const char* unit) {
  switch (unit[0]) {
    case 'C': return GAUGE_TYPE_TEMP;
    case 'W': return GAUGE_TYPE_POWER;
    case '%': return GAUGE_TYPE_PERCENT;
    default:  return GAUGE_TYPE_FAN;   // RPM, MB, MHz, ... -> generic value gauge
  }
}

static uint16_t blend565(uint8_t alpha, uint16_t fg, uint16_t bg) {
  const uint8_t r = ((fg >> 11) & 0x1F) * alpha / 255 +
                    ((bg >> 11) & 0x1F) * (255 - alpha) / 255;
  const uint8_t g = ((fg >> 5) & 0x3F) * alpha / 255 +
                    ((bg >> 5) & 0x3F) * (255 - alpha) / 255;
  const uint8_t b = (fg & 0x1F) * alpha / 255 +
                    (bg & 0x1F) * (255 - alpha) / 255;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t autoContrast565(uint16_t bg) {
  const uint16_t r = ((bg >> 11) & 0x1F) * 255 / 31;
  const uint16_t g = ((bg >> 5) & 0x3F) * 255 / 63;
  const uint16_t b = (bg & 0x1F) * 255 / 31;
  const uint16_t luminance = (uint16_t)((r * 54 + g * 183 + b * 19) / 256);
  return luminance > 128 ? 0x0000 : 0xFFFF;
}

static uint16_t themedLabelColor(uint16_t accent, uint16_t bg,
                                 uint16_t classicColor) {
  switch (themeSettings.labelMode) {
    case THEME_LABEL_CUSTOM: return themeSettings.labelColor;
    case THEME_LABEL_ACCENT: return accent;
    case THEME_LABEL_AUTO:   return autoContrast565(bg);
    default:                 return classicColor;
  }
}

static uint16_t themedTileColor(uint16_t accent) {
  if (themeSettings.tileTintPct == 0) return themeSettings.tileColor;
  const uint8_t alpha = (uint8_t)(themeSettings.tileTintPct * 255 / 100);
  return blend565(alpha, accent, themeSettings.tileColor);
}

struct MetricText {
  char value[12];
  char unit[8];
};

// One formatter feeds every monitor face so switching layouts never changes
// the meaning or precision of a reading. Large base-unit values use familiar
// compact forms while the companion protocol remains untouched.
static void formatMetricText(const PcMetric& metric, float raw, MetricText& out) {
  if (!isfinite(raw)) {
    strlcpy(out.value, "--", sizeof(out.value));
    out.unit[0] = '\0';
    return;
  }

  const float magnitude = fabsf(raw);
  strlcpy(out.unit, metric.unit, sizeof(out.unit));
  if (strcmp(metric.unit, "RPM") == 0 && magnitude >= 1000.0f) {
    snprintf(out.value, sizeof(out.value), "%.1fk", raw / 1000.0f);
  } else if (strcmp(metric.unit, "MHz") == 0 && magnitude >= 1000.0f) {
    snprintf(out.value, sizeof(out.value), "%.1f", raw / 1000.0f);
    strlcpy(out.unit, "GHz", sizeof(out.unit));
  } else if (strcmp(metric.unit, "MB") == 0 && magnitude >= 1024.0f) {
    snprintf(out.value, sizeof(out.value), "%.1f", raw / 1024.0f);
    strlcpy(out.unit, "GB", sizeof(out.unit));
  } else if (strcmp(metric.unit, "KB") == 0 && magnitude >= 1024.0f) {
    snprintf(out.value, sizeof(out.value), "%.1f", raw / 1024.0f);
    strlcpy(out.unit, "MB", sizeof(out.unit));
  } else if (strcmp(metric.unit, "W") == 0 && magnitude >= 1000.0f) {
    snprintf(out.value, sizeof(out.value), "%.1f", raw / 1000.0f);
    strlcpy(out.unit, "kW", sizeof(out.unit));
  } else if (strcmp(metric.unit, "V") == 0 || strcmp(metric.unit, "A") == 0 ||
             strcmp(metric.unit, "GHz") == 0 || strcmp(metric.unit, "GB") == 0) {
    snprintf(out.value, sizeof(out.value), "%.1f", raw);
  } else {
    snprintf(out.value, sizeof(out.value), "%.0f", raw);
  }
}

// ---------------------------------------------------------------------------
//  Alternate monitor styles (Big numbers / Tiles / Hero) - shared helpers.
//  All styles read the same GaugeMapping slots; slot order is reading order.
// ---------------------------------------------------------------------------
// RAII SPI-transaction bracket, same rationale as display_gauges' ScopedWrite:
// one transaction per style frame so WiFi/UDP servicing never interleaves
// between the primitives (the gauge-flicker chassis invariant).
class RendererWrite {
  lgfx::LovyanGFX& _t;
 public:
  explicit RendererWrite(lgfx::LovyanGFX& t) : _t(t) { _t.startWrite(); }
  ~RendererWrite() { _t.endWrite(); }
};

// True while the /screen.bmp handler renders into its capture sprite. The
// capture shares this renderer's code but must not consume the panel's
// pacing state (new-packet flags, layout counts, scale smoothing, previous
// text widths) - stealing those made the panel skip spark updates and
// rescale erratically whenever the portal preview auto-refreshed.
static bool gCaptureRender = false;
void setCaptureRender(bool on) { gCaptureRender = on; }

// True when the panel content was actually wiped (screen-state transition,
// style/status flip, online flip). Styles repaint static chrome (tile card
// backgrounds) only then - a plain forceDisplayRedraw() (e.g. after every
// preview capture) repaints values opaquely without blanking anything.
static bool gScreenCleared = true;
void markScreenCleared() {
  gScreenCleared = true;
  resetClock();
  resetMarioClock();
  resetPongClock();
}

// Sparkline compose sprite, file-scope so /api/status can report whether the
// flicker-free path is actually active on this board (a failed allocation
// silently falls back to direct clear+redraw, which blinks at packet rate).
static bool ensureSprite(lgfx::LGFX_Sprite& spr, int16_t& curW, int16_t& curH,
                         int16_t w, int16_t h) {
  if (curW == w && curH == h) return true;
  spr.deleteSprite();
  spr.setColorDepth(16);
  if (spr.createSprite(w, h)) { curW = w; curH = h; return true; }
  curW = curH = 0;
  return false;
}

static lgfx::LGFX_Sprite gSparkSpr;
static int16_t gSparkW = 0, gSparkH = 0;
static uint16_t gSparkFails = 0;
bool sparkSpriteActive() { return gSparkW > 0; }
uint16_t sparkSpriteFails() { return gSparkFails; }

// ---------------------------------------------------------------------------
//  Companion hiccup guards. The companion pushes ~every second; a packet that
//  momentarily drops metrics or flips the LHM status must not blank/relayout
//  the panel at packet rate (full-screen fillScreen at random intervals was
//  the "screen keeps flickering" report; count dips mid-relayout the
//  "graphs partially visible"). A change must persist across consecutive
//  frames before the layout reacts. Diagnostics count both raw wobble and
//  accepted transitions for /api/status.
// ---------------------------------------------------------------------------
static uint16_t gRawNChanges = 0, gRawStatusChanges = 0;
static uint16_t gRelayouts = 0, gStatusFlips = 0;
uint16_t rawNChanges()      { return gRawNChanges; }
uint16_t rawStatusChanges() { return gRawStatusChanges; }
uint16_t acceptedRelayouts()   { return gRelayouts; }
uint16_t acceptedStatusFlips() { return gStatusFlips; }

static uint8_t gStableN = 0xFF;
static uint8_t gStableStatus = 0xFF;

// Returns true when this frame may render; false = transient count wobble,
// skip the frame (pixels keep their last state for a few hundred ms).
static bool layoutCountReady(uint8_t n) {
  static uint8_t lastRawN = 0xFF, cand = 0xFF, cnt = 0;
  if (gCaptureRender) return true;
  if (lastRawN != n) { if (lastRawN != 0xFF) gRawNChanges++; lastRawN = n; }
  if (gStableN == 0xFF || n == gStableN) {
    gStableN = n;
    cand = 0xFF;
    cnt = 0;
    return true;
  }
  if (n == cand) {
    if (++cnt >= 3) {
      gStableN = n;
      cand = 0xFF;
      cnt = 0;
      gRelayouts++;
      return true;
    }
  } else {
    cand = n;
    cnt = 1;
  }
  return false;
}

// Debounced companion status for the badge + flip detection: a raw flap must
// persist ~5 frames before the screen reacts.
static uint8_t debouncedStatus() {
  static uint8_t lastRaw = 0xFF, cand = 0xFF, cnt = 0;
  const uint8_t cur = pcData.status;
  if (gCaptureRender) return (gStableStatus != 0xFF) ? gStableStatus : cur;
  if (lastRaw != cur) { if (lastRaw != 0xFF) gRawStatusChanges++; lastRaw = cur; }
  if (gStableStatus == 0xFF) { gStableStatus = cur; return cur; }
  if (cur == gStableStatus) {
    cand = 0xFF;
    cnt = 0;
  } else if (cur == cand) {
    if (++cnt >= 5) {
      gStableStatus = cur;
      cand = 0xFF;
      cnt = 0;
      gStatusFlips++;
    }
  } else {
    cand = cur;
    cnt = 1;
  }
  return gStableStatus;
}

struct VisSlot {
  uint8_t slotIdx;           // index into gaugeMap.slots / pcHistory
  const GaugeSlot* slot;
  const PcMetric* metric;
  const char* label;
};

// Bound slots whose metric is present in the live packet, in slot order.
static uint8_t collectVisibleSlots(VisSlot out[NUM_GAUGE_SLOTS]) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    const GaugeSlot& s = gaugeMap.slots[i];
    if (s.metricId == 0) continue;
    const PcMetric* m = pcMetricFindById(s.metricId);
    if (!m) continue;
    out[n].slotIdx = i;
    out[n].slot = &s;
    out[n].metric = m;
    out[n].label = gaugeDisplayLabel(i, m->name);
    n++;
  }
  return n;
}

// Full-scale for a slot's meter/fraction, mirroring drawSlotGauge's resolution
// so a bar shows the same fill the arc gauge would.
static float slotScaleMax(const GaugeSlot& slot, const PcMetric& m) {
  uint8_t type = slot.type;
  if (type == GAUGE_TYPE_AUTO || type >= GAUGE_TYPE_COUNT) type = classifyByUnit(m.unit);
  switch (type) {
    case GAUGE_TYPE_PERCENT: return 100.0f;
    case GAUGE_TYPE_POWER:
      return slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.powerScaleW;
    case GAUGE_TYPE_FAN:
      return slot.scaleMax ? (float)slot.scaleMax : (float)GAUGE_FAN_SCALE_DEFAULT;
    default:
      return slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.tempScaleMax;
  }
}

static float slotFraction(float value, float scale) {
  if (scale <= 0) return 0;
  float f = value / scale;
  return f < 0 ? 0 : (f > 1.0f ? 1.0f : f);
}

// Same warn rule the temp gauge applies (reading at or past warnThresholdPct
// percent of full-scale), but with hysteresis: enters at the threshold, exits
// 5 points below it, so a reading hovering around the line does not flap the
// warn color at the packet rate. Per-slot state.
static bool slotWarn(uint8_t slotIdx, float value, float scale) {
  static uint8_t state = 0;   // bitmask, NUM_GAUGE_SLOTS <= 8
  if (dispSettings.warnThresholdPct == 0 || scale <= 0) return false;
  const float pct = (value / scale) * 100.0f;
  const float enter = (float)dispSettings.warnThresholdPct;
  const bool on = state & (1u << slotIdx);
  const bool next = on ? (pct > enter - 5.0f) : (pct >= enter);
  if (!gCaptureRender) {
    if (next) state |= (1u << slotIdx);
    else      state &= ~(1u << slotIdx);
  }
  return next;
}

// Widest realistic reading for a slot ("3000" for a 3000 RPM full-scale).
// Fonts are sized against this probe instead of the live value, so a reading
// gaining or losing a digit between packets never makes the text jump sizes.
static void slotProbe(const GaugeSlot& s, const PcMetric& m, char* buf, size_t len) {
  MetricText scaleText, liveText;
  formatMetricText(m, slotScaleMax(s, m), scaleText);
  formatMetricText(m, m.value, liveText);
  const char* widest = strlen(liveText.value) > strlen(scaleText.value)
    ? liveText.value : scaleText.value;
  strlcpy(buf, widest, len);
}

// Pick the widest font (from base downwards) whose rendering of s fits maxW.
// Leaves the chosen font active. FONT_XLARGE quietly renders as FONT_LARGE on
// boards without the 22pt blob, which is exactly the wanted degradation.
static FontID fitFontForWidth(const char* s, int16_t maxW, FontID base) {
  static const FontID steps[] = { FONT_XLARGE, FONT_LARGE, FONT_BODY, FONT_SMALL };
  uint8_t i = 0;
  while (i < 3 && steps[i] != base) i++;
  setFont(tft, steps[i]);
  while (i < 3 && tft.textWidth(s) > maxW) setFont(tft, steps[++i]);
  return steps[i];
}

// True when the canvas is materially bigger than the 240-wide square panels -
// i.e. the 320x480 Guition in either orientation (min dimension 320), never a
// 240x240 or 240x320 board. Every layout change made for the big panel hangs
// off this flag as an added branch, so the 240x240 boards keep executing the
// exact code they always did rather than a "should be equivalent" rewrite.
static inline bool largeCanvas(int16_t w, int16_t h) {
  return (w < h ? w : h) >= 300;
}

// Pick the largest value face that fits natively, with NO setTextSize()
// magnification. Replaces the old fitFontForWidth + scaleValueToCell pair on
// every board: magnifying a VLW glyph stretches its 8-bit alpha ramp and looks
// blurry (up to 2x on the 240x240 panels, which is where it was worst), and the
// digits-only faces give real 30/38/48/68px glyphs instead. Constrains height as
// well as width, so a value only grows into room the cell actually has - which
// is what makes one ladder serve every panel size and metric count.
//
// DIGITS ONLY at the top four rungs - callers must pass a probe/value string
// from formatMetricText() or slotProbe(), never a label or unit.
// Takes the target explicitly so the sprite-composed faces can use it too.
// Leaves the chosen font ACTIVE at size 1.0 and returns it.
//
// Descent of a face, from its reported line box. Inter's descent is a stable
// 20-21% of fontHeight across every face in this project (3/15, 4/20, 6/28,
// 7/33, 8/38, 9/47, 12/59, 17/83), and LovyanGFX exposes no descent accessor.
static inline int16_t fontDescent(int16_t fontHeight) {
  return (int16_t)((fontHeight * 21) / 100);
}

// How far a value's glyph box may reach above its band. The value is drawn
// with an OPAQUE background, so the box - not the ink - is what lands on the
// panel, and the rows just above a band belong to the label's empty descent.
// Overrunning by that much is invisible; overrunning by more erases the label,
// which is exactly what an earlier ink-based fit did to the duo bands (it let
// a 59px face into a 47px band and the opaque box ate the name above it).
// FONT_SMALL is the label face everywhere here, so its descent is the budget.
static const int16_t VALUE_BOX_SLACK = 3;

// Baseline for a value fitted by fitValueFont, given the band below the label.
// Centers the glyph box in the band and pins its bottom to the band's bottom
// when the face is taller - the band plus the slack above it is the whole
// region the value may paint.
static inline int16_t valueBaseline(int16_t gapY, int16_t band, int16_t fh) {
  const int16_t baseY = gapY + (band + fh) / 2;
  return (baseY > gapY + band) ? (int16_t)(gapY + band) : baseY;
}

static FontID fitValueFont(lgfx::LovyanGFX& gfx, const char* s,
                           int16_t maxW, int16_t maxH) {
  static const FontID ladder[] = {
    FONT_NUM_XXL, FONT_NUM_XL, FONT_NUM_L, FONT_NUM_M,
    FONT_XLARGE, FONT_LARGE, FONT_BODY, FONT_SMALL
  };
  const uint8_t last = (uint8_t)(sizeof(ladder) / sizeof(ladder[0]) - 1);
  gfx.setTextSize(1.0f);
  for (uint8_t i = 0; i < last; i++) {
    setFont(gfx, ladder[i]);
    if ((int16_t)gfx.textWidth(s) <= maxW &&
        (int16_t)gfx.fontHeight() <= maxH + VALUE_BOX_SLACK) {
      return ladder[i];
    }
  }
  setFont(gfx, ladder[last]);
  return ladder[last];
}

// Upgrade the unit face beside a fitted value. The value is always fitted
// against the FONT_SMALL unit reservation first; a bigger unit face is chosen
// only when the leftover width absorbs the growth and the value glyph stays
// taller than the unit. So a narrow % grows beside big digits while a wide
// RPM simply stays small - the value never shrinks for its unit. Leaves the
// font state changed; the caller re-applies the value font afterwards.
static FontID upgradeUnitFont(const char* unit, int16_t slackW,
                              int16_t valueFh, int16_t smallUnitW) {
  if (!unit || !unit[0]) return FONT_SMALL;
  if (valueFh >= 35) {
    setFont(tft, FONT_LARGE);
    if ((int16_t)tft.textWidth(unit) - smallUnitW <= slackW) return FONT_LARGE;
  }
  if (valueFh >= 24) {
    setFont(tft, FONT_BODY);
    if ((int16_t)tft.textWidth(unit) - smallUnitW <= slackW) return FONT_BODY;
  }
  return FONT_SMALL;
}

// Horizontal meter: full-width track + fraction fill, rounded ends.
static void drawMeterBar(int16_t x, int16_t y, int16_t w, int16_t h,
                         float frac, uint16_t color) {
  int16_t r = h / 2;
  tft.fillRoundRect(x, y, w, h, r, dispSettings.trackColor);
  int16_t fw = (int16_t)(frac * w + 0.5f);
  if (fw > 0) {
    if (fw < h) fw = h;   // keep the rounded cap from degenerating
    tft.fillRoundRect(x, y, fw, h, r, color);
  }
}

// Chart core shared by the sparkline path and the strips lanes: smoothed
// bounds plus the column-wise area/line render, drawn into any target (panel
// or a compose sprite) at the given offset. The caller owns the background.
// Smoothed per-slot chart bounds, shared by the flat sparkline and the glass
// chart so switching faces never rescales a series differently. Expands to the
// window immediately (a new extreme must be visible at once) and contracts
// slowly (5% per redraw), so the chart does not rescale-jump every time an
// extreme leaves the ring. Returns false when there is nothing to plot yet.
static bool sparkBounds(const SlotHistory& hist, uint8_t slotIdx, bool advance,
                        float& lo, float& hi) {
  if (hist.count < 2) return false;
  float wlo = pcHistoryAt(hist, 0), whi = wlo;
  for (uint8_t i = 1; i < hist.count; i++) {
    float v = pcHistoryAt(hist, i);
    if (v < wlo) wlo = v;
    if (v > whi) whi = v;
  }
  // Most sensors arrive quantised (whole percent, whole RPM), and auto-scaling
  // a nearly flat window amplifies ONE quantum into a huge vertical jump: at 60
  // samples across ~105 px a run of equal readings is a 2 px wide plateau, so
  // the series reads as a staircase of blocks rather than a line. Estimate the
  // quantum from the smallest real change in the ring and keep several of them
  // in view. A genuinely continuous sensor has a tiny quantum, so this costs it
  // no detail.
  float quantum = 0.0f;
  for (uint8_t i = 1; i < hist.count; i++) {
    float d = pcHistoryAt(hist, i) - pcHistoryAt(hist, i - 1);
    if (d < 0) d = -d;
    if (d > 1e-4f && (quantum == 0.0f || d < quantum)) quantum = d;
  }
  const float minSpan = quantum * 6.0f;
  if (minSpan > 0.0f && (whi - wlo) < minSpan) {
    const float mid = (whi + wlo) * 0.5f;
    wlo = mid - minSpan * 0.5f;
    whi = mid + minSpan * 0.5f;
  }

  float pad = (whi - wlo) * 0.15f;
  if (pad < 0.5f) pad = 0.5f;
  wlo -= pad;
  whi += pad;

  static float sLo[NUM_GAUGE_SLOTS], sHi[NUM_GAUGE_SLOTS];
  static uint8_t sInit = 0;
  const uint8_t bit = (uint8_t)(1u << slotIdx);
  if (gCaptureRender) {
    lo = (sInit & bit) ? sLo[slotIdx] : wlo;
    hi = (sInit & bit) ? sHi[slotIdx] : whi;
    return true;
  }
  // Mutate the smoothed bounds only when a new sample arrived - a plain full
  // redraw (portal preview cycle) must reproduce identical pixels, otherwise
  // the chart creeps every 4 s while the preview is open.
  if (hist.count <= 2 || !(sInit & bit)) {
    sLo[slotIdx] = wlo;
    sHi[slotIdx] = whi;
    sInit |= bit;
  } else if (advance) {
    if (wlo < sLo[slotIdx]) sLo[slotIdx] = wlo;
    else sLo[slotIdx] += (wlo - sLo[slotIdx]) * 0.05f;
    if (whi > sHi[slotIdx]) sHi[slotIdx] = whi;
    else sHi[slotIdx] += (whi - sHi[slotIdx]) * 0.05f;
  }
  lo = sLo[slotIdx];
  hi = sHi[slotIdx];
  return true;
}

// Defined with the chart helpers further down; the flat faces and the glass
// faces share both so a smoothing setting means the same thing on every face.
static uint8_t buildChartSeries(const SlotHistory& hist, float* out, uint8_t passes);
static float histSmooth(const float* s, int n, float fi);

static void sparkPlot(lgfx::LovyanGFX& g, const SlotHistory& hist,
                      uint8_t slotIdx, int16_t ox, int16_t oy,
                      int16_t w, int16_t h, uint16_t color, bool advance,
                      uint16_t scrollQ8 = 0) {
  float lo, hi;
  if (sparkBounds(hist, slotIdx, advance, lo, hi)) {
    // Column-wise render: a dim area fill under a 2px connected line. A bare
    // 1px polyline disappears on the physical panel wherever the series goes
    // flat near the box edge; the filled area keeps the shape readable.
    // Same series treatment as the glass charts: an own copy so the optional
    // low pass cannot touch the ring the readings print from, sampled with the
    // Catmull-Rom rather than a bare lerp.
    float series[PC_HISTORY_LEN];
    const int n = (int)buildChartSeries(hist, series, chartSmoothing);
    const int16_t dotR = (h >= 24) ? 2 : 1;
    const int16_t plotW = w - dotR - 1;   // reserve so the endpoint dot stays inside
    const uint16_t areaColor = (uint16_t)((color >> 2) & 0x39E7);  // ~1/4 brightness
    // Fixed samples per pixel, newest pinned to the right edge, so a partly
    // filled ring grows in from the right instead of re-fitting itself across
    // the full width on every packet. Both callers clear the box before this,
    // so the columns ahead of xStart can simply be left alone.
    const float pdenom = (plotW > 1) ? (float)(plotW - 1) : 1.0f;
    const float pstep = (float)(PC_HISTORY_LEN - 1) / pdenom;
    // Lag one sample and slide across the packet interval so the newest
    // reading walks in instead of popping: at scroll 0 the window ends on the
    // second-newest, at full scroll exactly on the newest.
    const bool glide = (n >= 3);
    const float shift = glide ? ((float)scrollQ8 / 256.0f) : 0.0f;
    const float pRight = (glide ? (float)(n - 2) : (float)(n - 1)) + shift;
    int16_t xStart = (int16_t)(plotW - 1 - (int32_t)(pRight / pstep));
    if (xStart < 0) xStart = 0;
    int16_t prevY = 0, sy = 0;
    for (int16_t xi = xStart; xi < plotW; xi++) {
      const float fi = pRight - (float)(plotW - 1 - xi) * pstep;
      const float v = histSmooth(series, n, fi < 0.0f ? 0.0f : fi);
      sy = oy + (h - 1) - (int16_t)((v - lo) * (float)(h - 1) / (hi - lo));
      g.drawFastVLine(ox + xi, sy, oy + h - sy, areaColor);
      const int16_t lineTop = (xi > xStart && prevY < sy) ? prevY : sy;
      const int16_t lineBot = (xi > xStart && prevY > sy) ? prevY : sy;
      int16_t lineH = lineBot - lineTop + 2;   // 2px stroke
      if (lineTop + lineH > oy + h) lineH = oy + h - lineTop;  // stay inside the box
      g.drawFastVLine(ox + xi, lineTop, lineH, color);
      prevY = sy;
    }
    // Keep the endpoint dot fully inside the box (matters on the direct-draw
    // fallback, which has no sprite clipping under it).
    int16_t dotY = sy;
    if (dotY < oy + dotR) dotY = oy + dotR;
    if (dotY > oy + h - 1 - dotR) dotY = oy + h - 1 - dotR;
    g.fillCircle(ox + plotW - 1, dotY, dotR, themeSettings.valueColor);
  }
}

// Translucent scrim behind overlay text. The strips lanes print their label
// and reading on top of the chart, and a bright area fill swallows both (a
// panel photo of the CLK and VRAM lanes made that plain). Blending the pixels
// under the type toward the background gives it a surface to sit on while the
// chart shape stays visible underneath - a solid box would punch a hole in the
// chart, and a drawn outline turns to mush at 12px.
//
// Sprite targets only. This reads back what was just drawn, which is a RAM
// access on a sprite but a slow per-pixel bus read on the panel (and not all
// panels can read at all), so the no-sprite fallback keeps drawing over the
// chart the way it always did.
static void drawTextScrim(lgfx::LGFX_Sprite& spr, int16_t x, int16_t y,
                          int16_t w, int16_t h, uint16_t bg, uint8_t alpha) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > (int16_t)spr.width())  w = (int16_t)spr.width() - x;
  if (y + h > (int16_t)spr.height()) h = (int16_t)spr.height() - y;
  if (w <= 0 || h <= 0) return;

  // Clipped corners so the shade reads as a soft panel rather than a hard box.
  const int16_t r = (w > 8 && h > 8) ? 3 : 0;
  for (int16_t yy = 0; yy < h; yy++) {
    const int16_t dy = (yy < r) ? (int16_t)(r - yy)
                     : (yy >= h - r) ? (int16_t)(yy - (h - 1 - r)) : (int16_t)0;
    for (int16_t xx = 0; xx < w; xx++) {
      const int16_t dx = (xx < r) ? (int16_t)(r - xx)
                       : (xx >= w - r) ? (int16_t)(xx - (w - 1 - r)) : (int16_t)0;
      if (dx && dy && dx * dx + dy * dy > r * r) continue;
      const int16_t px = x + xx, py = y + yy;
      spr.drawPixel(px, py, blend565(alpha, bg, spr.readPixel(px, py)));
    }
  }
}

// Sparkline over a slot's history ring, scaled to the local min/max (padded so
// a flat line does not hug an edge), 1px polyline with a bright endpoint dot.
// Composed in a reusable offscreen sprite and pushed in one blit so the
// clear+redraw never flashes at the packet cadence; falls back to direct
// drawing (slightly blinky) only if the sprite cannot be allocated.
static void drawSparkline(const SlotHistory& hist, uint8_t slotIdx,
                          int16_t x, int16_t y,
                          int16_t w, int16_t h, uint16_t color, uint16_t bg,
                          bool advance, uint16_t scrollQ8 = 0) {
  if (w < 8 || h < 8) return;

  if (gSparkW != w || gSparkH != h) {
    gSparkSpr.deleteSprite();
    gSparkSpr.setColorDepth(16);
    if (gSparkSpr.createSprite(w, h)) { gSparkW = w; gSparkH = h; }
    else { gSparkW = gSparkH = 0; gSparkFails++; }
  }
  const bool off = (gSparkW == w && gSparkH == h);
  lgfx::LovyanGFX& g = off ? (lgfx::LovyanGFX&)gSparkSpr : (lgfx::LovyanGFX&)tft;
  const int16_t ox = off ? 0 : x;
  const int16_t oy = off ? 0 : y;

  g.fillRect(ox, oy, w, h, bg);
  sparkPlot(g, hist, slotIdx, ox, oy, w, h, color, advance, scrollQ8);
  if (off) {
    gSparkSpr.pushSprite(tft_ptr, x, y);
    // pushSprite may queue the transfer via SPI DMA and return; the shared
    // sprite gets refilled for the NEXT chart immediately after. Without this
    // barrier the in-flight transfer reads the half-overwritten buffer -
    // tiles then show each other's colors and wedge-shaped partial fills.
    tft.waitDMA();
  }
}

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

// Working colour space for the compositor. int16_t rather than uint8_t so an
// intermediate blend can overshoot without wrapping.
struct Rgb { int16_t r, g, b; };

static inline Rgb rgbFrom565(uint16_t c) {
  return Rgb{ (int16_t)((((c >> 11) & 0x1F) * 255 + 15) / 31),
              (int16_t)((((c >> 5) & 0x3F) * 255 + 31) / 63),
              (int16_t)(((c & 0x1F) * 255 + 15) / 31) };
}

// alpha is the weight of b, 0..255.
static inline Rgb rgbMix(const Rgb& a, const Rgb& b, uint8_t alpha) {
  return Rgb{ (int16_t)(a.r + (((int32_t)b.r - a.r) * alpha >> 8)),
              (int16_t)(a.g + (((int32_t)b.g - a.g) * alpha >> 8)),
              (int16_t)(a.b + (((int32_t)b.b - a.b) * alpha >> 8)) };
}

static const Rgb RGB_WHITE = { 255, 255, 255 };
static const Rgb RGB_BLACK = { 0, 0, 0 };

// 8x8 ordered dither, applied at the 8888 -> 565 store so a long ramp turns
// its band edges into a stipple the eye integrates back into a smooth
// gradient. Without it the backdrop shows every one of its ~32 blue steps.
//
// 8x8 rather than 4x4: both kill the banding, but a 4x4 cell at this pixel
// pitch reads as a visible checkerboard on the panel. The threshold is also
// CENTRED (-32..31) instead of added, so dithering does not lift the whole
// image half a quantisation step brighter.
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

static inline uint16_t rgbTo565(const Rgb& c, int16_t x, int16_t y) {
  const int32_t t = (int32_t)kBayer8[((y & 7) << 3) | (x & 7)] - 32;
  // One quantisation step of spread: the 5-bit channels step by 8, the 6-bit
  // green by 4. Any more is noise, any less leaves the band edge visible.
  int32_t r = c.r + (t >> 3);
  int32_t g = c.g + (t >> 4);
  int32_t b = c.b + (t >> 3);
  if (r < 0) r = 0; else if (r > 255) r = 255;
  if (g < 0) g = 0; else if (g > 255) g = 255;
  if (b < 0) b = 0; else if (b > 255) b = 255;
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Compose target. Writing straight into a sprite's buffer instead of going
// through drawPixel is what makes a fully per-pixel glass face affordable on
// the C3, which has no FPU and pays for every call. buf == nullptr falls back
// to the generic path so the no-sprite case still renders.
struct GlassCanvas {
  lgfx::LovyanGFX* g;
  uint16_t* buf;
  int32_t stride;
  int16_t w, h;
  int16_t ox, oy;      // where this canvas sits, for the generic path
};

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

static GlassCanvas glassCanvasFor(lgfx::LovyanGFX& g, lgfx::LGFX_Sprite* spr,
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

// Derived from the user's bgColor so the Colors card still means something:
// the ramp is that colour lifted toward a cool slate at the top and sunk
// toward black at the bottom, never a hardcoded palette.
static void glassSkyInit(int16_t h, const Rgb& tint,
                         uint8_t topA, uint8_t midA, uint8_t botA) {
  const Rgb base = rgbFrom565(dispSettings.bgColor);
  gSky.top = rgbMix(base, tint, topA);
  gSky.mid = rgbMix(base, tint, midA);
  gSky.bot = rgbMix(base, RGB_BLACK, botA);
  gSky.h = h > 1 ? h : 1;
}

// Two-segment ramp: the knee at 55% keeps the upper half bright enough for the
// panes to read as sitting ON something while the floor still goes properly
// dark behind the lowest cards.
static inline Rgb glassSkyAt(int16_t y) {
  if (y < 0) y = 0;
  if (y >= gSky.h) y = gSky.h - 1;
  const int32_t knee = ((int32_t)gSky.h * 55) / 100;
  if (y <= knee) {
    const uint8_t a = knee ? (uint8_t)(((int32_t)y * 255) / knee) : 0;
    return rgbMix(gSky.top, gSky.mid, a);
  }
  const int32_t span = gSky.h - 1 - knee;
  const uint8_t a = span > 0 ? (uint8_t)((((int32_t)y - knee) * 255) / span) : 255;
  return rgbMix(gSky.mid, gSky.bot, a);
}

// One dithered row at a time, pushed as an image. drawFastHLine would be one
// call per row and would band; per-pixel drawPixel would be 57600 windowed SPI
// writes and take seconds. Composing the row in RAM and blitting it is both
// smooth and fast.
static void glassBackdrop(int16_t w, int16_t h) {
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

struct GlassStyle {
  uint8_t lift;      // pane body pulled toward white (the "smoke")
  uint8_t tint;      // slot accent bled into the body
  uint8_t bloom;     // accent glow rising off the bottom edge
  uint8_t glossA;    // specular peak alpha, 0 = no gloss at all
  uint8_t glossPct;  // specular band height as a % of pane height
  uint8_t refract;   // rim samples the sky this many rows lower, 0 = off
  uint8_t rimTop;
  uint8_t rimSide;
  int8_t  rimBot;    // > 0 lifts toward white, < 0 sinks toward black
};

// Vista: polished. A bright specular arc, a hard bevel underneath, warm bloom.
static const GlassStyle GLASS_AERO = {
  /*lift*/ 26, /*tint*/ 30, /*bloom*/ 30, /*glossA*/ 88, /*glossPct*/ 44,
  /*refract*/ 0, /*rimTop*/ 120, /*rimSide*/ 34, /*rimBot*/ -76
};
// Modern: edge-lit, no gloss, light wrapping under the bottom edge.
static const GlassStyle GLASS_LIQUID = {
  /*lift*/ 30, /*tint*/ 26, /*bloom*/ 18, /*glossA*/ 0, /*glossPct*/ 0,
  /*refract*/ 6, /*rimTop*/ 132, /*rimSide*/ 72, /*rimBot*/ 54
};

// Pane body colour for one row. Pure function of the panel row, which is what
// lets two sprites compose adjacent slices of the same pane and join invisibly.
static inline Rgb glassPaneRow(int16_t panelY, int16_t dy, int16_t paneH,
                               const Rgb& accent, const GlassStyle& gs) {
  Rgb c = rgbMix(glassSkyAt(panelY), RGB_WHITE, gs.lift);
  c = rgbMix(c, accent, gs.tint);
  if (gs.bloom && paneH > 1) {
    const int32_t t = ((int32_t)dy * 255) / (paneH - 1);
    if (t > 140) c = rgbMix(c, accent, (uint8_t)(((int32_t)gs.bloom * (t - 140)) / 115));
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
static void glassPaneWindow(const GlassCanvas& c,
                            int16_t wx, int16_t wy, int16_t ww, int16_t wh,
                            int16_t paneY, int16_t paneW, int16_t paneH,
                            int16_t r, const Rgb& accent, const GlassStyle& gs,
                            const Rgb* warnRim = nullptr) {
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
    const Rgb rimSrc = gs.refract
      ? rgbMix(glassSkyAt(panelRow + gs.refract), RGB_WHITE, (uint8_t)(gs.lift + 24))
      : body;
    Rgb rimTopC  = rgbMix(rimSrc, RGB_WHITE, gs.rimTop);
    Rgb rimSideC = rgbMix(rimSrc, RGB_WHITE, gs.rimSide);
    Rgb rimBotC  = gs.rimBot >= 0
      ? rgbMix(body, RGB_WHITE, (uint8_t)gs.rimBot)
      : rgbMix(body, RGB_BLACK, (uint8_t)(-gs.rimBot));
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
    int32_t glossF = 0;
    if (glossH > 0 && dy < glossH) {
      const int32_t f = 255 - ((int32_t)dy * 255) / glossH;
      glossF = (f * f) / 255;                       // quadratic, no hard edge
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
        // Parabolic gloss: brightest at the pane's centre column, fading to
        // nothing at the sides - the Vista arc.
        const int32_t t = ((pxC - cxQ8) * 255) / (cxQ8 > 0 ? cxQ8 : 1);
        int32_t shape = 255 - (t * t * 140) / (255 * 255);
        if (shape > 0) {
          const int32_t al = ((int32_t)gs.glossA * glossF / 255) * shape / 255;
          if (al > 0) col = rgbMix(col, RGB_WHITE, (uint8_t)(al > 255 ? 255 : al));
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

// --- motion -----------------------------------------------------------------
//
// The companion pushes roughly once a second. Stepping the plot a whole column
// per packet is a visible jerk, so the chart glides by a fraction of a sample
// between packets and the newest reading slides in over the interval. That
// costs one packet of latency at the right edge, which is invisible, and is
// what buys continuous motion instead of a 1 Hz stutter.
//
// The reading itself is NOT eased. Running the digits through intermediate
// values would make them change on every frame instead of once per packet -
// more churn, and a stats readout that shows numbers the sensor never
// reported. Motion belongs to the chart; the number stays exact.
static uint32_t gPktIntervalMs = 1000;
static uint32_t gPrevRx = 0;
static uint16_t gScrollQ8 = 0;
static bool gNewSample = false;

static void advanceChartMotion() {
  if (gCaptureRender) return;      // capture must not consume pacing state
  const uint32_t rx = pcData.lastReceived;
  gNewSample = false;
  if (rx == 0) { gScrollQ8 = 0; return; }
  if (rx != gPrevRx) {
    if (gPrevRx != 0) {
      const uint32_t dt = rx - gPrevRx;
      if (dt >= 150 && dt <= 10000) gPktIntervalMs = (gPktIntervalMs * 3 + dt) / 4;
    }
    gPrevRx = rx;
    gNewSample = true;
  }
  const uint32_t elapsed = millis() - rx;
  const uint32_t span = gPktIntervalMs ? gPktIntervalMs : 1000;
  const uint32_t q = (elapsed * 256) / span;
  gScrollQ8 = (uint16_t)(q > 256 ? 256 : q);
}

// --- chart ------------------------------------------------------------------

// Catmull-Rom sample with a monotone clamp. Only visibly different from a
// straight lerp while the ring is still filling (few samples across many
// columns), but that is exactly when the stepped polyline looks worst; the
// clamp stops the spline overshooting a percentage below zero.
// Copy the ring oldest->newest, optionally low-passed. Each pass is a binomial
// [1,2,1]/4 kernel, which rounds one-sample needles without moving the series
// sideways the way a trailing average would. The two ENDPOINTS are left exact
// so the live end of the chart still agrees with the printed reading.
static uint8_t buildChartSeries(const SlotHistory& hist, float* out, uint8_t passes) {
  const uint8_t n = hist.count;
  for (uint8_t i = 0; i < n; i++) out[i] = pcHistoryAt(hist, i);
  if (n < 3) return n;
  for (uint8_t p = 0; p < passes; p++) {
    float prev = out[0];
    for (uint8_t i = 1; i + 1 < n; i++) {
      const float cur = out[i];
      out[i] = (prev + 2.0f * cur + out[i + 1]) * 0.25f;
      prev = cur;
    }
  }
  return n;
}

static float histSmooth(const float* s, int n, float fi) {
  int i1 = (int)fi;
  if (i1 < 0) i1 = 0;
  if (i1 > n - 1) i1 = n - 1;
  const float t = fi - (float)i1;
  const int i0 = i1 > 0 ? i1 - 1 : 0;
  const int i2 = i1 + 1 < n ? i1 + 1 : n - 1;
  const int i3 = i1 + 2 < n ? i1 + 2 : n - 1;
  const float p0 = s[i0], p1 = s[i1];
  const float p2 = s[i2], p3 = s[i3];
  const float v = 0.5f * ((2.0f * p1) +
                          (-p0 + p2) * t +
                          (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                          (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t * t * t);
  // Clamp to the four-point neighbourhood, NOT to [p1,p2]. Pinning to the
  // segment holds every run of equal samples perfectly flat and then jumps at
  // the riser, which is exactly what turns a quantised series into flat blocks.
  // The wider bound lets a plateau ease into the next one while still keeping
  // the spline inside the data, so it cannot overshoot below zero either.
  float lo = p0, hi = p0;
  if (p1 < lo) lo = p1;
  if (p1 > hi) hi = p1;
  if (p2 < lo) lo = p2;
  if (p2 > hi) hi = p2;
  if (p3 < lo) lo = p3;
  if (p3 > hi) hi = p3;
  return v < lo ? lo : (v > hi ? hi : v);
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
static void glassChart(const GlassCanvas& c, const SlotHistory& hist,
                       uint8_t slotIdx, int16_t ox, int16_t oy,
                       int16_t w, int16_t h,
                       const Rgb& paneAccent, const Rgb& lineAccent,
                       int16_t paneY, int16_t paneLocalY,
                       int16_t paneH, int16_t fadeIn,
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
  const Rgb lineInk = rgbMix(lineAccent, RGB_WHITE, 40);
  const uint8_t AREA_TOP = 150;   // fill alpha just under the stroke
  const uint8_t AREA_BOT = 26;    // ...and at the floor of the plot

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

    for (int16_t yy = 0; yy < h; yy++) {
      const int32_t rowT = ((int32_t)yy) << 8, rowB = rowT + 256;

      // Whatever the pane would have been at this pixel is the chart's ground.
      Rgb base = rowBase[yy];

      // Area fill under the stroke, fading with depth.
      int32_t al = -1;
      if (rowT >= fillTop) {
        const int32_t depth =
          hQ8 > fillTop ? ((rowT - fillTop) * 255) / (hQ8 - fillTop + 1) : 255;
        al = AREA_TOP - ((AREA_TOP - AREA_BOT) * depth) / 255;
      } else if (rowB > fillTop) {
        // Partial row at the fill's top edge.
        al = (AREA_TOP * ((rowB - fillTop) * 255 / 256)) / 255;
      }
      if (al > 0) {
        al = (al * edgeA) / 255;
        base = rgbMix(base, lineAccent, (uint8_t)(al > 255 ? 255 : al));
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
        const Rgb dot = rgbMix(rowBase[qy - oy], RGB_WHITE, al);
        gcPixel(c, qx, qy, rgbTo565(dot, qx, qy));
      }
    }
  }
}

// ---------------------------------------------------------------------------
//  Flicker-free value regions. The old scheme cleared the whole cell/row and
//  repainted it, which blinks at the packet cadence. Instead: opaque glyphs
//  overwrite the previous text in one pass, and only the pixels the new text
//  no longer covers get cleared - painting bg over bg is invisible, so the
//  only pixels that visibly change are the ones that actually changed.
// ---------------------------------------------------------------------------
// Left-aligned value + a small dim unit set against the value height.
// Caller has already selected the value font (fitValueFont); resets the text
// size to 1.0 itself. bandH covers the tallest glyph box a previous draw may
// have used.
static void drawValueRegionL(int16_t x, int16_t baseY, int16_t bandW, int16_t bandH,
                             const char* val, const char* unit,
                             uint16_t fg, uint16_t bg, int16_t* prevVw,
                             FontID unitFont = FONT_SMALL) {
  const int16_t fh = (int16_t)tft.fontHeight();
  // A face can report more height than the band holds - its descent is empty
  // for digits, which is why fitValueFont let it through. Clip every clear to
  // the band so the region never paints over the label above it.
  const int16_t boxH = (fh > bandH) ? bandH : fh;
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(val, x, baseY);
  const int16_t vw = tft.textWidth(val);
  tft.setTextSize(1.0f);
  // The band above the glyph box is background whenever the region is
  // healthy, so repainting it is invisible - and it evicts stale pixels left
  // by an earlier taller font or a layout that once painted there.
  if (bandH > boxH) tft.fillRect(x, baseY - bandH, bandW, bandH - boxH, bg);
  // The unit and the vacated-pixel clears only need repainting when the
  // value's glyph box changed - doing it every update made the unit blink at
  // the packet rate. The key folds in the font height so a font change with
  // an equal pixel width still repaints (the unit rides the glyph height).
  // prevVw==nullptr forces a full repaint without touching state (capture).
  const int16_t key = (int16_t)(uint16_t)((vw & 0x01FF) | ((fh & 0x7F) << 9));
  if (prevVw && *prevVw == key) return;
  if (bandW > vw) tft.fillRect(x + vw, baseY - boxH, bandW - vw, boxH, bg);
  if (unit && unit[0]) {
    setFont(tft, unitFont);
    const int16_t unitFh = (int16_t)tft.fontHeight();
    // Centering the unit against the glyph box reads fine when the two faces
    // are close in size, but a floating mid-height unit looks detached once the
    // value towers over it. Sit it on the value's baseline then, the way "15 %"
    // is normally set. The large canvas always does this (it never renders a
    // small value here); the square panels now reach 38-59px values too, so
    // they get the same treatment whenever the gap is wide.
    //
    // BL_DATUM places a box BOTTOM, not a baseline, so passing baseY straight
    // through drops the unit by the difference in descents - about 9px under a
    // 59px value, which reads as the unit having slipped below the number.
    // Lift it by that difference so the two baselines actually meet.
    const bool baselineUnit =
        largeCanvas((int16_t)tft.width(), (int16_t)tft.height()) ||
        (fh - unitFh) >= 16;
    const int16_t unitBaseY =
        baselineUnit ? (int16_t)(baseY - (fontDescent(fh) - fontDescent(unitFh)))
                     : (int16_t)(baseY - (fh - unitFh) / 2);
    tft.setTextDatum(BL_DATUM);
    tft.setTextColor(themeSettings.secondaryColor, bg);
    tft.drawString(unit, x + vw + 5, unitBaseY);
  }
  if (prevVw) *prevVw = key;
}

// Right-aligned (middle datum) value whose left edge moves as digits change:
// clears only the vacated pixels between leftBound and the new glyph box.
static void drawValueRegionR(int16_t leftBound, int16_t rightX, int16_t cy,
                             int16_t bandH, const char* s,
                             uint16_t fg, uint16_t bg) {
  const int16_t fh = (int16_t)tft.fontHeight();
  const int16_t boxTop = cy - fh / 2;
  const int16_t bandTop = cy - bandH / 2;
  const int16_t regionW = rightX - leftBound;
  if (regionW <= 0) return;
  if (boxTop > bandTop) {
    tft.fillRect(leftBound, bandTop, regionW, boxTop - bandTop, bg);
    tft.fillRect(leftBound, boxTop + fh, regionW, bandTop + bandH - boxTop - fh, bg);
  }
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(s, rightX, cy);
  const int16_t vw = tft.textWidth(s);
  if (regionW > vw) tft.fillRect(leftBound, boxTop, regionW - vw, fh, bg);
}

static void drawNoMetricsHint(int16_t w, int16_t gridH, bool fr) {
  if (!fr) return;
  tft.setTextDatum(MC_DATUM);
  setFont(tft, FONT_BODY);
  // Transparent on glass: the opaque form stamps a flat box through the
  // gradient. The glass faces paint their backdrop before calling this.
  if (styleUsesGlass(displayStyle)) tft.setTextColor(themeSettings.secondaryColor);
  else tft.setTextColor(themeSettings.secondaryColor, dispSettings.bgColor);
  tft.drawString("No metrics bound", w / 2, gridH / 2);
}

// ---------------------------------------------------------------------------
//  STYLE_BIG_NUMBERS - uppercase label, one large tabular value, hairline
//  meter carrying the same fraction the arc would. Hairline separators
//  instead of cards keep every pixel for type.
// ---------------------------------------------------------------------------
static void drawBigNumbersScreen(bool fr) {
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
    snprintf(key, sizeof(key), "%s%s", text.value, warn ? "!" : "");
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
    const FontID vf = fitValueFont(tft, probe, availW, band);
    const int16_t valueFh = (int16_t)tft.fontHeight();
    const int16_t baseY = valueBaseline(gapY, band, valueFh);
    const int16_t bandH = baseY - gapY;
    const int16_t slackW = availW - (int16_t)tft.textWidth(probe);
    // Keep the unit a clear step below the value so it stays subordinate.
    const FontID uf = big ? ((valueFh >= 60) ? FONT_XLARGE : FONT_LARGE)
                          : upgradeUnitFont(text.unit, slackW, valueFh, unitW);
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
//  STYLE_TILES - one card per metric: name + value on the head line, a
//  sparkline of the slot's history filling the rest. Warn recolors the line
//  and adds a stripe on the card's left edge.
// ---------------------------------------------------------------------------
static void drawTilesScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;

  VisSlot vis[NUM_GAUGE_SLOTS];
  uint8_t n = collectVisibleSlots(vis);
  if (!layoutCountReady(n)) return;

  static uint8_t lastN = 0xFF;
  bool relayout = false;
  if (!gCaptureRender && n != lastN) {
    if (!fr) {
      tft.fillRect(0, 0, w, gridH, bg);
      resetGaugeTextCache();
    }
    lastN = n;
    fr = true;
    relayout = true;
  }

  if (n == 0) {
    drawNoMetricsHint(w, gridH, fr);
    return;
  }

  // Sparklines repaint at a calm cadence, not per packet: the companion can
  // push every second, and repainting ~25% of every chart's pixels each
  // second reads as screen-wide shimmer on the panel. Values stay live; the
  // charts advance every SPARK_REDRAW_MS. Capture never consumes the pacing.
  // Charts animate at the frame rate, exactly like the glass faces: the plot
  // glides a fraction of a sample between packets instead of stepping a whole
  // column once per sparkRedrawSec. `advance` still means "a new reading
  // landed", which is the only thing the bounds smoothing may react to, and
  // the reading itself is never eased - motion belongs to the chart.
  advanceChartMotion();
  const bool sparkTick = true;
  const bool advance = gCaptureRender ? false : gNewSample;
  const uint16_t scroll = gCaptureRender ? 0 : gScrollQ8;

  const uint8_t cols = (n <= 2) ? 1 : 2;
  const uint8_t rows = (n + cols - 1) / cols;
  const int16_t pad = 4, gap = 4;
  const int16_t cardW = (w - 2 * pad - (cols - 1) * gap) / cols;
  const int16_t cardH = (gridH - 2 * pad - (rows - 1) * gap) / rows;
  int16_t headH = (cardH * 2) / 5;
  if (headH < 22) headH = 22;

  RendererWrite rw(tft);

  for (uint8_t i = 0; i < n; i++) {
    const int16_t x = pad + (i % cols) * (cardW + gap);
    const int16_t y = pad + (i / cols) * (cardH + gap);
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);
    // The chart wears the slot's identity color unconditionally: a warn flip
    // must not recolor three minutes of history (stripe + value carry it).
    const uint16_t lineColor = s.arcColor;
    const uint16_t cardBg = themedTileColor(lineColor);
    const uint16_t labelColor = themedLabelColor(lineColor, cardBg, CLR_TEXT_DIM);

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s", text.value, warn ? "!" : "");
    const bool head = gaugeTextChanged(x + cardW / 2, y, key, label, fr);

    // Card background only when something actually blanked the area - a
    // plain full redraw (preview capture, style save) repaints content
    // opaquely and refilling the card here would flash it empty first.
    if (fr && (gScreenCleared || relayout || gCaptureRender))
      tft.fillRoundRect(x, y, cardW, cardH, 6, cardBg);

    if (head) {
      // Compose the whole head strip offscreen and push it as ONE blit - the
      // atomic path leaves nothing to blink, exactly like the spark sprite.
      static lgfx::LGFX_Sprite headSpr;
      static int16_t hsW = 0, hsH = 0;
      if (ensureSprite(headSpr, hsW, hsH, cardW, headH)) {
        resetFontCache();   // fonts were loaded on the panel, retarget them
        headSpr.fillSprite(bg);   // panel bg shows through the corner notches
        // Card top with rounded corners; drawn taller than the strip so the
        // bottom edge clips square and joins the card body seamlessly.
        headSpr.fillRoundRect(0, 0, cardW, headH + 8, 6, cardBg);
        if (warn) headSpr.fillRect(0, 6, 3, headH - 6, dispSettings.warnColor);

        const int16_t headCy = headH / 2 + 1;
        setFont(headSpr, FONT_SMALL);
        const int16_t labelW = headSpr.textWidth(label);
        const int16_t unitW  = headSpr.textWidth(text.unit);
        headSpr.setTextDatum(ML_DATUM);
        headSpr.setTextColor(labelColor, cardBg);
        headSpr.drawString(label, 9, headCy);
        headSpr.setTextDatum(MR_DATUM);
        headSpr.setTextColor(themeSettings.secondaryColor, cardBg);
        headSpr.drawString(text.unit, cardW - 9, headCy);

        char probe[12];
        slotProbe(s, m, probe, sizeof(probe));
        // Width-fit the probe on the sprite (same glyph metrics as the panel).
        {
          static const FontID steps[] = { FONT_XLARGE, FONT_LARGE, FONT_BODY, FONT_SMALL };
          uint8_t fi = 0;
          FontID base = (headH >= 34) ? FONT_LARGE : FONT_BODY;
          while (fi < 3 && steps[fi] != base) fi++;
          setFont(headSpr, steps[fi]);
          const int16_t maxW = cardW - 30 - labelW - unitW;
          while (fi < 3 && headSpr.textWidth(probe) > maxW) setFont(headSpr, steps[++fi]);
        }
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
        char probe[12];
        slotProbe(s, m, probe, sizeof(probe));
        fitFontForWidth(probe, cardW - 30 - labelW - unitW,
                         (headH >= 34) ? FONT_LARGE : FONT_BODY);
        drawValueRegionR(x + 9 + labelW + 6, x + cardW - 9 - unitW - 4, headCy,
                         headH - 4, text.value,
                         warn ? dispSettings.warnColor : themeSettings.valueColor,
                         cardBg);
      }
      // Warn stripe below the head strip (small overpaint, cannot blink).
      tft.fillRect(x, y + headH, 3, cardH - headH - 6,
                   warn ? dispSettings.warnColor : cardBg);
    }

    if ((sparkTick || fr) && cardH - headH - 10 >= 8) {
      drawSparkline(pcHistory[vis[i].slotIdx], vis[i].slotIdx, x + 8, y + headH + 2,
                    cardW - 16, cardH - headH - 10, lineColor, cardBg, advance, scroll);
    }
  }
}

// ---------------------------------------------------------------------------
//  STYLE_HERO - slot 1 owns the top band: large value + its sparkline. The
//  remaining metrics compress into rows (name, thin bar, value). With a
//  single bound metric the sparkline takes the whole area under the band.
// ---------------------------------------------------------------------------
static void drawHeroScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;
  const bool big = largeCanvas(w, h);

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

  // 2/5 of a 240px panel is 96px, which the hero band needs. The same ratio on
  // a 480px panel is 192px and reads as a slab: the value and chart do not grow
  // proportionally, so the extra height is mostly air while the rows below get
  // squeezed. Give the big canvas a smaller share.
  const int16_t heroH = big ? (gridH * 30) / 100 : (gridH * 2) / 5;
  const PcMetric& hm = *vis[0].metric;
  const GaugeSlot& hs = *vis[0].slot;
  const char* heroLabel = vis[0].label;
  const float heroScale = slotScaleMax(hs, hm);
  const bool heroWarn = slotWarn(vis[0].slotIdx, hm.value, heroScale);
  const uint16_t heroColor = hs.arcColor;   // chart keeps identity; value carries warn

  RendererWrite rw(tft);

  MetricText heroText;
  formatMetricText(hm, hm.value, heroText);
  char key[16];
  snprintf(key, sizeof(key), "%s%s", heroText.value, heroWarn ? "!" : "");
  // Anchor off-grid (3, heroH) so it can never collide with a row anchor.
  if (gaugeTextChanged(3, heroH, key, heroLabel, fr)) {
    const int16_t heroW = (n >= 2) ? (w / 2) : w;

    setFont(tft, FONT_SMALL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(themedLabelColor(hs.arcColor, bg, CLR_TEXT_DARK), bg);
    tft.drawString(heroLabel, 12, 8);
    const int16_t lw = tft.textWidth(heroLabel);
    const int16_t lfh = (int16_t)tft.fontHeight();
    tft.fillRect(12 + lw, 8, heroW - 24 - lw, lfh, bg);
    // Strip between the label bottom and the value band: nothing legitimate
    // renders there, so a background repaint is invisible and evicts residue
    // left by an earlier layout or screen.
    const int16_t gapY = 8 + lfh;

    // Unit reserved at final size on the big canvas - see the big-numbers cells.
    if (big) setFont(tft, FONT_XLARGE);
    const int16_t unitW = tft.textWidth(heroText.unit);
    const int16_t availW = heroW - 24 - unitW - 6;
    char probe[12];
    slotProbe(hs, hm, probe, sizeof(probe));
    // Native face + glyph box centered in the band under the label, same
    // treatment as the big-numbers cells. bandH is pinned to (baseY - gapY) so
    // drawValueRegionL's clear stops below the label.
    const int16_t band = heroH - 12 - gapY;
    const FontID vf = fitValueFont(tft, probe, availW, band);
    const int16_t valueFh = (int16_t)tft.fontHeight();
    const int16_t baseY = valueBaseline(gapY, band, valueFh);
    const int16_t bandH = baseY - gapY;
    const int16_t slackW = availW - (int16_t)tft.textWidth(probe);
    const FontID uf = big ? ((valueFh >= 60) ? FONT_XLARGE : FONT_LARGE)
                          : upgradeUnitFont(heroText.unit, slackW, valueFh, unitW);
    setFont(tft, vf);
    static int16_t heroPrevVw = -1;
    if (fr && !gCaptureRender) heroPrevVw = -1;
    drawValueRegionL(12, baseY, heroW - 24, bandH,
                     heroText.value, heroText.unit,
                     heroWarn ? dispSettings.warnColor : themeSettings.valueColor, bg,
                     gCaptureRender ? nullptr : &heroPrevVw, uf);
  }

  if (fr) tft.drawFastHLine(8, heroH, w - 16, dispSettings.trackColor);

  // Hero sparkline: extend slightly into the center gap while preserving a
  // clear margin after a three-digit hero value and its unit.
  if (sparkTick || fr) {
    if (n >= 2) {
      const int16_t graphX = w / 2 - 10;
      drawSparkline(pcHistory[vis[0].slotIdx], vis[0].slotIdx, graphX, 10,
                    w - graphX - 10, heroH - 20, heroColor, bg, advance, scroll);
    } else {
      drawSparkline(pcHistory[vis[0].slotIdx], vis[0].slotIdx, 12, heroH + 8,
                    w - 24, gridH - heroH - 16, heroColor, bg, advance, scroll);
    }
  }

  if (n < 2) return;

  // Rows for the remaining metrics. The bar begins closer to the labels and
  // reaches farther right, leaving a stable value area at the panel edge.
  const int16_t rowsY0 = heroH + 2;
  const int16_t rowH = (gridH - rowsY0) / (n - 1);
  // Label column. w/4 leaves only 64px of text room at 320px wide, which a
  // 4-character label like "CPUW" overflows at FONT_LARGE - fitFontForWidth then
  // drops it to FONT_BODY, and because the ladder mixes weights (inter_19 is
  // Inter-Bold, inter_14 is Inter-Regular) the label visibly loses its bold as
  // well as its size, so one row looks wrong next to the others. Widen the
  // column on the big canvas so the common labels never need to step down.
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
  for (uint8_t i = 1; i < n; i++) {
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const float frac = slotFraction(m.value, scale);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);
    const int16_t rowY = rowsY0 + (i - 1) * rowH;
    const int16_t cy = rowY + rowH / 2;

    MetricText rowText;
    formatMetricText(m, m.value, rowText);
    char rkey[16];
    snprintf(rkey, sizeof(rkey), "%s%s", rowText.value, warn ? "!" : "");
    if (!gaugeTextChanged(w / 2, cy, rkey, label, fr)) continue;

    fitFontForWidth(label, bx - 16, rowLabelFont);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(themedLabelColor(s.arcColor, bg, CLR_TEXT_DIM), bg);
    tft.drawString(label, 8, cy);

    drawMeterBar(bx, cy - bh / 2, bw, bh, frac,
                 warn ? dispSettings.warnColor : s.arcColor);

    char vb[20];
    snprintf(vb, sizeof(vb), "%s %s", rowText.value, rowText.unit);
    MetricText probeText;
    formatMetricText(m, scale, probeText);
    char valueProbe[20];
    snprintf(valueProbe, sizeof(valueProbe), "%s %s",
             probeText.value, probeText.unit);
    fitFontForWidth(valueProbe, rowValueW,
                    big ? ((rowH >= 44) ? FONT_XLARGE : FONT_LARGE)
                        : ((rowH >= 34) ? FONT_LARGE : FONT_BODY));
    drawValueRegionR(valueLeft, valueRight, cy, rowH - 2, vb,
                     warn ? dispSettings.warnColor : themeSettings.valueColor, bg);
  }
}

// ---------------------------------------------------------------------------
//  STYLE_STRIPS - one full-width sparkline lane per metric, the label and
//  reading laid over the chart. Each lane composes offscreen and pushes as one
//  blit; because the text sits on the chart, the whole lane repaints at the
//  chart cadence (sparkRedrawSec), so readings ride that pace by design.
// ---------------------------------------------------------------------------
// How far the scrim under the lane text pulls the chart back toward the
// background. High enough that a full-height area fill stops competing with
// the type, low enough that the chart's shape still shows through it.
static const uint8_t STRIPS_SCRIM_ALPHA = 205;

static void drawStripsScreen(bool fr) {
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

  for (uint8_t i = 0; i < n; i++) {
    const int16_t y = i * rowH;
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);
    const uint16_t lineColor = s.arcColor;

    MetricText text;
    formatMetricText(m, m.value, text);
    char probe[12];
    slotProbe(s, m, probe, sizeof(probe));

    lgfx::LovyanGFX& g = off ? (lgfx::LovyanGFX&)rowSpr : (lgfx::LovyanGFX&)tft;
    const int16_t oy = off ? 0 : y;

    if (off) {
      resetFontCache();   // fonts were loaded on the panel, retarget them
      rowSpr.fillSprite(bg);
    } else {
      tft.fillRect(0, y, w, rowH, bg);
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

    {
      static const FontID steps[] = { FONT_XLARGE, FONT_LARGE, FONT_BODY, FONT_SMALL };
      uint8_t fi = 0;
      FontID base = (rowH >= 34) ? FONT_LARGE : FONT_BODY;
      while (fi < 3 && steps[fi] != base) fi++;
      setFont(g, steps[fi]);
      const int16_t maxW = w / 2 - unitW - 12;
      while (fi < 3 && g.textWidth(probe) > maxW) setFont(g, steps[++fi]);
    }
    const int16_t valueRight = w - 6 - unitW - 5;
    const int16_t valueW = g.textWidth(text.value);
    const int16_t valueH = (int16_t)g.fontHeight();
    const int16_t cy = oy + rowH / 2;

    if (off) {
      drawTextScrim(rowSpr, 4, oy + 1, labelW + 5, labelH + 4, bg, STRIPS_SCRIM_ALPHA);
      const int16_t readLeft = valueRight - valueW - 5;
      drawTextScrim(rowSpr, readLeft, cy - valueH / 2 - 2,
                    w - 2 - readLeft, valueH + 4, bg, STRIPS_SCRIM_ALPHA);
    }

    g.setTextDatum(MR_DATUM);
    g.setTextColor(warn ? dispSettings.warnColor : themeSettings.valueColor);
    g.drawString(text.value, valueRight, cy);
    setFont(g, FONT_SMALL);
    g.setTextColor(themeSettings.secondaryColor);
    g.drawString(text.unit, w - 6, cy);
    g.setTextDatum(TL_DATUM);
    g.setTextColor(themedLabelColor(lineColor, bg, CLR_TEXT_DIM));
    g.drawString(label, 6, oy + 3);

    if (off) {
      rowSpr.pushSprite(tft_ptr, 0, y);
      tft.waitDMA();      // shared sprite: barrier before the next lane refill
      resetFontCache();   // next panel setFont must reload onto the panel
    }
  }
}

// ---------------------------------------------------------------------------
//  STYLE_DUO - slots 1 and 2 each get a hero band (label, large value, chart);
//  the remaining metrics condense into a two-column grid with meter bars.
//  With one bound metric the band sits on top and the chart fills the rest.
// ---------------------------------------------------------------------------
static void drawDuoScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;
  const bool big = largeCanvas(w, h);

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

  const uint8_t bands = (n >= 2) ? 2 : 1;
  // Two bands at 32% each take 64% of the panel. That is fine at 240px, but on
  // a 480px panel it leaves the 2x2 meter grid cramped against the bottom edge
  // while the bands themselves hold mostly empty space. See drawHeroScreen.
  const int16_t bandH = (n == 1) ? (big ? (gridH * 30) / 100 : (gridH * 2) / 5)
                       : (n == 2) ? (big ? (gridH * 40) / 100 : gridH / 2)
                       : (big ? (gridH * 25) / 100 : (gridH * 32) / 100);
  const int16_t chartX = w / 2 - 2;

  RendererWrite rw(tft);

  for (uint8_t b = 0; b < bands; b++) {
    const int16_t y = b * bandH;
    const PcMetric& m = *vis[b].metric;
    const GaugeSlot& s = *vis[b].slot;
    const char* label = vis[b].label;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(vis[b].slotIdx, m.value, scale);

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s", text.value, warn ? "!" : "");
    if (gaugeTextChanged(3, y + bandH, key, label, fr)) {
      // Tight margins: the band value competes with the chart for width, so
      // the value region keeps only 10+4 px side padding and an 8 px bottom
      // margin - together with the quarter scale steps this is what lets a
      // four-digit RPM reading render a full step larger.
      const int16_t valueW = (n == 1) ? w : chartX;

      setFont(tft, FONT_SMALL);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(themedLabelColor(s.arcColor, bg, CLR_TEXT_DARK), bg);
      tft.drawString(label, 10, y + 6);
      const int16_t lw = tft.textWidth(label);
      const int16_t lfh = (int16_t)tft.fontHeight();
      tft.fillRect(10 + lw, y + 6, valueW - 14 - lw, lfh, bg);
      const int16_t gapY = y + 6 + lfh;

      // Unit reserved at final size on the big canvas - see the big-numbers cells.
      if (big) setFont(tft, FONT_XLARGE);
      const int16_t unitW = tft.textWidth(text.unit);
      const int16_t availW = valueW - 14 - unitW - 5;
      char probe[12];
      slotProbe(s, m, probe, sizeof(probe));
      // Native face, box centered under the label. See the big-numbers cells
      // for why bandV is pinned to (baseY - gapY).
      const int16_t band = (y + bandH - 8) - gapY;
      const FontID vf = fitValueFont(tft, probe, availW, band);
      const int16_t valueFh = (int16_t)tft.fontHeight();
      const int16_t baseY = valueBaseline(gapY, band, valueFh);
      const int16_t bandV = baseY - gapY;
      const int16_t slackW = availW - (int16_t)tft.textWidth(probe);
      const FontID uf = big ? ((valueFh >= 60) ? FONT_XLARGE : FONT_LARGE)
                            : upgradeUnitFont(text.unit, slackW, valueFh, unitW);
      setFont(tft, vf);
      static int16_t bandPrevVw[2] = { -1, -1 };
      if (fr && !gCaptureRender) bandPrevVw[b] = -1;
      drawValueRegionL(10, baseY, valueW - 14, bandV,
                       text.value, text.unit,
                       warn ? dispSettings.warnColor : themeSettings.valueColor, bg,
                       gCaptureRender ? nullptr : &bandPrevVw[b], uf);
    }

    if (fr) tft.drawFastHLine(8, y + bandH - 1, w - 16, dispSettings.trackColor);

    if (sparkTick || fr) {
      if (n >= 2) {
        drawSparkline(pcHistory[vis[b].slotIdx], vis[b].slotIdx,
                      chartX, y + 8, w - chartX - 8, bandH - 18,
                      s.arcColor, bg, advance, scroll);
      } else {
        drawSparkline(pcHistory[vis[b].slotIdx], vis[b].slotIdx,
                      12, bandH + 8, w - 24, gridH - bandH - 16,
                      s.arcColor, bg, advance, scroll);
      }
    }
  }

  if (n <= 2) return;

  // Grid for the remaining metrics: label left, value right, meter beneath.
  const int16_t y0 = 2 * bandH + 2;
  const uint8_t rest = n - 2;
  const uint8_t rows = (rest + 1) / 2;
  const int16_t cellH = (gridH - y0) / rows;
  const int16_t cellW = w / 2;
  for (uint8_t i = 0; i < rest; i++) {
    const PcMetric& m = *vis[2 + i].metric;
    const GaugeSlot& s = *vis[2 + i].slot;
    const char* label = vis[2 + i].label;
    const float scale = slotScaleMax(s, m);
    const float frac = slotFraction(m.value, scale);
    const bool warn = slotWarn(vis[2 + i].slotIdx, m.value, scale);
    const int16_t x = (i % 2) * cellW;
    const int16_t y = y0 + (i / 2) * cellH;
    const int16_t cy = y + (cellH - 10) / 2;

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s", text.value, warn ? "!" : "");
    if (!gaugeTextChanged(x + cellW / 2, cy, key, label, fr)) continue;

    // Same reasoning as the hero rows: a 160px-wide cell on the large canvas
    // dwarfed the SMALL label / BODY value ceiling. Unit is in the string, so
    // full-charset faces only.
    setFont(tft, big ? FONT_BODY : FONT_SMALL);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(themedLabelColor(s.arcColor, bg, CLR_TEXT_DIM), bg);
    tft.drawString(label, x + 10, cy);
    const int16_t labelW = tft.textWidth(label);

    char vb[20];
    snprintf(vb, sizeof(vb), "%s %s", text.value, text.unit);
    MetricText probeText;
    formatMetricText(m, scale, probeText);
    char valueProbe[20];
    snprintf(valueProbe, sizeof(valueProbe), "%s %s",
             probeText.value, probeText.unit);
    // LARGE, not XLARGE: these cells are only ~160px wide and the value shares
    // the row with the label, so an XLARGE 4-digit value plus unit ("462 MB")
    // butts straight into a 4-character label. Still a clear step up from the
    // original BODY ceiling.
    fitFontForWidth(valueProbe, cellW - 26 - labelW,
                    big ? FONT_LARGE : FONT_BODY);
    drawValueRegionR(x + 10 + labelW + 6, x + cellW - 10, cy, cellH - 14, vb,
                     warn ? dispSettings.warnColor : themeSettings.valueColor, bg);

    drawMeterBar(x + 10, y + cellH - 9, cellW - 20, 3, frac,
                 warn ? dispSettings.warnColor : s.arcColor);
  }
}

// ---------------------------------------------------------------------------
//  STYLE_PULSE - one accent-washed block per metric; the wash intensity
//  follows the load fraction (quantized with hysteresis so blocks do not
//  shimmer at the packet rate). Built for reading the machine's state from
//  across the room rather than reading digits.
// ---------------------------------------------------------------------------
static void drawPulseScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t gridH = h;
  const uint16_t bg = dispSettings.bgColor;
  const bool big = largeCanvas(w, h);

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

  const uint8_t cols = (n <= 2) ? 1 : 2;
  const uint8_t rows = (n + cols - 1) / cols;
  const int16_t cellW = w / cols;
  const int16_t cellH = gridH / rows;
  const int16_t blockW = cellW - 4;
  const int16_t blockH = cellH - 4;

  RendererWrite rw(tft);

  static uint8_t lastQ[NUM_GAUGE_SLOTS];
  static uint8_t qInit = 0;

  for (uint8_t i = 0; i < n; i++) {
    const int16_t x = (i % cols) * cellW;
    const int16_t y = (i / cols) * cellH;
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const float frac = slotFraction(m.value, scale);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);
    const uint8_t slotIdx = vis[i].slotIdx;
    const uint8_t bit = (uint8_t)(1u << slotIdx);

    // Quantized wash intensity, 8 steps, with hysteresis: leave the current
    // step only when the fraction moves clearly past the boundary, so a value
    // hovering on a step edge does not flip the block back and forth.
    const float qf = frac * 8.0f;
    uint8_t q;
    if ((qInit & bit) && fabsf(qf - (float)lastQ[slotIdx]) < 0.62f) {
      q = lastQ[slotIdx];
    } else {
      q = (uint8_t)(qf + 0.5f);
      if (q > 8) q = 8;
    }
    if (!gCaptureRender) { lastQ[slotIdx] = q; qInit |= bit; }

    const uint8_t alpha = warn ? (uint8_t)(77 + q * 14) : (uint8_t)(26 + q * 13);
    const uint16_t base = warn ? dispSettings.warnColor : s.arcColor;
    const uint16_t cellBg = blend565(alpha, base, bg);
    const uint16_t textC = autoContrast565(cellBg);
    const uint16_t labelC = blend565(200, textC, cellBg);

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s%u", text.value, warn ? "!" : "", q);
    if (!gaugeTextChanged(x + cellW / 2, y + cellH / 2, key, label, fr)) continue;

    char probe[12];
    slotProbe(s, m, probe, sizeof(probe));

    static lgfx::LGFX_Sprite blockSpr;
    static int16_t bsW = 0, bsH = 0;
    if (ensureSprite(blockSpr, bsW, bsH, blockW, blockH)) {
      resetFontCache();   // fonts were loaded on the panel, retarget them
      blockSpr.fillSprite(bg);
      blockSpr.fillRoundRect(0, 0, blockW, blockH, 5, cellBg);

      setFont(blockSpr, big ? FONT_BODY : FONT_SMALL);
      blockSpr.setTextDatum(TL_DATUM);
      blockSpr.setTextColor(labelC, cellBg);
      blockSpr.drawString(label, 9, 7);
      const int16_t labelBot = 7 + (int16_t)blockSpr.fontHeight();

      // Unit is measured on the label face (FONT_SMALL / FONT_BODY) because
      // that is what renders it below.
      setFont(blockSpr, FONT_SMALL);
      if (big) setFont(blockSpr, FONT_XLARGE);
      const int16_t unitW = blockSpr.textWidth(text.unit);
      const int16_t maxW = blockW - 18 - unitW - 5;
      // Native digits instead of a magnified glyph, box centered between the
      // label and the block bottom - the same treatment the other faces get,
      // applied to the sprite this one composes into.
      const int16_t band = (blockH - 8) - labelBot;
      fitValueFont(blockSpr, probe, maxW, band);
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

      blockSpr.pushSprite(tft_ptr, x + 2, y + 2);
      tft.waitDMA();      // shared sprite: barrier before the next block refill
      resetFontCache();   // next panel setFont must reload onto the panel
    } else {
      // Sprite unavailable: direct paint (blinks on wash step change only).
      tft.fillRoundRect(x + 2, y + 2, blockW, blockH, 5, cellBg);
      setFont(tft, FONT_SMALL);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(labelC, cellBg);
      tft.drawString(label, x + 11, y + 9);
      fitFontForWidth(probe, blockW - 18, FONT_XLARGE);
      tft.setTextDatum(BL_DATUM);
      tft.setTextColor(textC, cellBg);
      tft.drawString(text.value, x + 11, y + 2 + blockH - 8);
      const int16_t vw = tft.textWidth(text.value);
      setFont(tft, FONT_SMALL);
      tft.setTextColor(labelC, cellBg);
      tft.drawString(text.unit, x + 11 + vw + 5, y + 2 + blockH - 8);
      tft.setTextDatum(TL_DATUM);
    }
  }
}

// ---------------------------------------------------------------------------
//  STYLE_GLASS_TILES / STYLE_GLASS_DUO
//
//  Both faces reuse the geometry of the flat face they are based on and swap
//  the surface underneath it. Two shared compose sprites serve every window on
//  the panel; they are released when the style changes away so the two glass
//  faces never hold each other's buffers.
// ---------------------------------------------------------------------------
static lgfx::LGFX_Sprite gGlassA, gGlassB;
static int16_t gGlassAW = 0, gGlassAH = 0, gGlassBW = 0, gGlassBH = 0;

static void glassReleaseSprites() {
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

// Label ink on glass: mostly white with enough of the slot accent to identify
// the series, which survives the lifted pane where a dim grey would not.
static inline uint16_t glassLabelInk(uint16_t accent565) {
  const Rgb c = rgbMix(RGB_WHITE, rgbFrom565(accent565), 88);
  return rgbTo565(c, 0, 0);
}

static const Rgb GLASS_TINT_AERO   = { 44, 96, 150 };
static const Rgb GLASS_TINT_LIQUID = { 66, 62, 134 };

// Value/unit/label onto an already-composed glass window. Transparent text:
// the two-argument setTextColor paints an opaque glyph box, which on glass
// stamps a flat rectangle through the pane. The window is recomposed every
// time it is pushed, so there is no previous text to erase anyway.
static void glassHeadText(lgfx::LovyanGFX& g, int16_t x, int16_t cy,
                          int16_t w, int16_t maxH, const char* label,
                          const MetricText& text,
                          const GaugeSlot& s, const PcMetric& m, bool warn,
                          uint16_t accent) {
  setFont(g, FONT_SMALL);
  const int16_t labelW = g.textWidth(label);
  const int16_t unitW  = g.textWidth(text.unit);

  g.setTextDatum(ML_DATUM);
  g.setTextColor(glassLabelInk(accent));
  g.drawString(label, x + 9, cy);
  g.setTextDatum(MR_DATUM);
  g.setTextColor(themeSettings.secondaryColor);
  g.drawString(text.unit, x + w - 9, cy);

  char probe[12];
  slotProbe(s, m, probe, sizeof(probe));
  fitValueFont(g, probe, w - 30 - labelW - unitW, maxH);
  g.setTextDatum(MR_DATUM);
  g.setTextColor(warn ? dispSettings.warnColor : themeSettings.valueColor);
  g.drawString(text.value, x + w - 9 - unitW - 4, cy);
  g.setTextDatum(TL_DATUM);
}

static void drawGlassTilesScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const uint16_t bg = dispSettings.bgColor;

  VisSlot vis[NUM_GAUGE_SLOTS];
  uint8_t n = collectVisibleSlots(vis);
  if (!layoutCountReady(n)) return;

  glassSkyInit(h, GLASS_TINT_AERO, 150, 62, 90);

  static uint8_t lastN = 0xFF;
  bool relayout = false;
  if (!gCaptureRender && n != lastN) {
    if (!fr) { resetGaugeTextCache(); }
    lastN = n;
    fr = true;
    relayout = true;
  }

  if (n == 0) {
    if (fr) { glassBackdrop(w, h); drawNoMetricsHint(w, h, fr); }
    return;
  }

  // Glass charts glide continuously rather than stepping every sparkRedrawSec:
  // they are recomposed each frame at the faster glass cadence, and only the
  // bounds smoothing is gated on an actual new packet.
  advanceChartMotion();
  const bool chartTick = true;
  const bool advance = gCaptureRender ? false : gNewSample;
  const uint16_t scroll = gCaptureRender ? 0 : gScrollQ8;

  const uint8_t cols = (n <= 2) ? 1 : 2;
  const uint8_t rows = (n + cols - 1) / cols;
  const int16_t pad = 4, gap = 4;
  const int16_t cardW = (w - 2 * pad - (cols - 1) * gap) / cols;
  const int16_t cardH = (h - 2 * pad - (rows - 1) * gap) / rows;
  int16_t headH = (cardH * 2) / 5;
  if (headH < 24) headH = 24;
  const int16_t bodyH = cardH - headH;

  RendererWrite rw(tft);

  // The backdrop is only repainted when something actually blanked the panel;
  // the panes themselves are opaque blits that cover their own ground.
  if (fr && (gScreenCleared || relayout || gCaptureRender)) glassBackdrop(w, h);

  const int16_t sprH = headH > bodyH ? headH : bodyH;
  const bool off = ensureSprite(gGlassA, gGlassAW, gGlassAH, cardW, sprH);

  for (uint8_t i = 0; i < n; i++) {
    const int16_t x = pad + (i % cols) * (cardW + gap);
    const int16_t y = pad + (i / cols) * (cardH + gap);
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);
    const uint16_t accent565 = s.arcColor;
    // One accent everywhere on the card - body, chart ground and series ink.
    // Warn is carried by the lit rim and the value colour, never by retinting
    // the body, which would leave the chart looking like a foreign rectangle.
    const Rgb paneAccent = rgbFrom565(accent565);
    const Rgb lineAccent = paneAccent;
    const Rgb warnRgb = rgbFrom565(dispSettings.warnColor);
    const Rgb* warnRim = warn ? &warnRgb : nullptr;

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s", text.value, warn ? "!" : "");
    const bool headChanged = gaugeTextChanged(x + cardW / 2, y, key, label, fr);

    if (headChanged) {
      if (off) {
        resetFontCache();
        GlassCanvas c = glassCanvasFor(gGlassA, &gGlassA, 0, 0);
        glassPaneWindow(c, 0, 0, cardW, headH, y, cardW, cardH, 7,
                        paneAccent, GLASS_AERO, warnRim);
        glassHeadText(gGlassA, 0, headH / 2 + 1, cardW, headH - 6, label,
                      text, s, m, warn, accent565);
        glassBlit(gGlassA, x, y, cardW, headH);
        resetFontCache();
      } else {
        GlassCanvas c = glassCanvasFor(tft, nullptr, x, y);
        glassPaneWindow(c, 0, 0, cardW, headH, y, cardW, cardH, 7,
                        paneAccent, GLASS_AERO, warnRim);
        glassHeadText(tft, x, y + headH / 2 + 1, cardW, headH - 6, label,
                      text, s, m, warn, accent565);
      }
    }

    if ((chartTick || fr) && bodyH > 10) {
      if (off) {
        // oy = -headH maps pane row headH onto sprite row 0, so the body
        // window is composed at the top of the same buffer the head used.
        GlassCanvas c = glassCanvasFor(gGlassA, &gGlassA, 0, -headH);
        glassPaneWindow(c, 0, headH, cardW, bodyH, y, cardW, cardH, 7,
                        paneAccent, GLASS_AERO, warnRim);
        glassChart(c, pcHistory[vis[i].slotIdx], vis[i].slotIdx,
                   7, headH + 2, cardW - 14, bodyH - 8,
                   paneAccent, lineAccent, y, headH + 2, cardH, 0,
                   GLASS_AERO, advance, scroll);
        glassBlit(gGlassA, x, y + headH, cardW, bodyH);
      } else {
        GlassCanvas c = glassCanvasFor(tft, nullptr, x, y);
        glassPaneWindow(c, 0, headH, cardW, bodyH, y, cardW, cardH, 7,
                        paneAccent, GLASS_AERO, warnRim);
        glassChart(c, pcHistory[vis[i].slotIdx], vis[i].slotIdx,
                   7, headH + 2, cardW - 14, bodyH - 8,
                   paneAccent, lineAccent, y, headH + 2, cardH, 0,
                   GLASS_AERO, advance, scroll);
      }
    }
  }
  (void)bg;
}

static void drawGlassDuoScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const bool big = largeCanvas(w, h);

  VisSlot vis[NUM_GAUGE_SLOTS];
  uint8_t n = collectVisibleSlots(vis);
  if (!layoutCountReady(n)) return;

  glassSkyInit(h, GLASS_TINT_LIQUID, 134, 54, 96);

  static uint8_t lastN = 0xFF;
  bool relayout = false;
  if (!gCaptureRender && n != lastN) {
    if (!fr) { resetGaugeTextCache(); }
    lastN = n;
    fr = true;
    relayout = true;
  }

  if (n == 0) {
    if (fr) { glassBackdrop(w, h); drawNoMetricsHint(w, h, fr); }
    return;
  }

  advanceChartMotion();
  const bool chartTick = true;
  const bool advance = gCaptureRender ? false : gNewSample;
  const uint16_t scroll = gCaptureRender ? 0 : gScrollQ8;

  RendererWrite rw(tft);
  if (fr && (gScreenCleared || relayout || gCaptureRender)) glassBackdrop(w, h);

  const int16_t margin = 5, vgap = 4;
  const uint8_t bands = (n >= 2) ? 2 : 1;
  const int16_t paneW = w - 2 * margin;
  // Band height backs off as pill rows are added. At eight metrics two bands
  // at 31% left the three pill rows 22px each, which clipped their labels.
  const uint8_t prowsPlanned = (n > 2) ? (uint8_t)((n - 2 + 1) / 2) : 0;
  const uint8_t bandPct = (n == 1) ? (big ? 34 : 42)
                        : (n == 2) ? (big ? 42 : 48)
                        : (prowsPlanned <= 1) ? (big ? 30 : 34)
                        : (prowsPlanned == 2) ? (big ? 26 : 30)
                                              : (big ? 22 : 26);
  const int16_t bandH = (int16_t)((h * bandPct) / 100);
  const int16_t radius = 16;
  const int16_t halfW = paneW / 2;

  const bool off = ensureSprite(gGlassA, gGlassAW, gGlassAH, halfW, bandH);

  for (uint8_t b = 0; b < bands; b++) {
    const int16_t y = margin + b * (bandH + vgap);
    const PcMetric& m = *vis[b].metric;
    const GaugeSlot& s = *vis[b].slot;
    const char* label = vis[b].label;
    const float scale = slotScaleMax(s, m);
    const bool warn = slotWarn(vis[b].slotIdx, m.value, scale);
    const uint16_t accent565 = s.arcColor;
    const Rgb paneAccent = rgbFrom565(accent565);
    const Rgb lineAccent = paneAccent;
    const Rgb warnRgb = rgbFrom565(dispSettings.warnColor);
    const Rgb* warnRim = warn ? &warnRgb : nullptr;

    MetricText text;
    formatMetricText(m, m.value, text);
    char key[16];
    snprintf(key, sizeof(key), "%s%s", text.value, warn ? "!" : "");
    const bool textChanged = gaugeTextChanged(3, y + bandH, key, label, fr);

    // Left capsule half: name and reading.
    if (textChanged) {
      lgfx::LovyanGFX& g = off ? (lgfx::LovyanGFX&)gGlassA : (lgfx::LovyanGFX&)tft;
      const int16_t gx = off ? 0 : margin, gy = off ? 0 : y;
      if (off) resetFontCache();
      GlassCanvas c = off ? glassCanvasFor(gGlassA, &gGlassA, 0, 0)
                          : glassCanvasFor(tft, nullptr, margin, y);
      glassPaneWindow(c, 0, 0, halfW, bandH, y, paneW, bandH, radius,
                      paneAccent, GLASS_LIQUID, warnRim);

      setFont(g, FONT_SMALL);
      g.setTextDatum(TL_DATUM);
      g.setTextColor(glassLabelInk(accent565));
      g.drawString(label, gx + 15, gy + 9);
      const int16_t lfh = (int16_t)g.fontHeight();

      char probe[12];
      slotProbe(s, m, probe, sizeof(probe));
      setFont(g, FONT_SMALL);
      const int16_t unitW = g.textWidth(text.unit);
      const int16_t gapY = gy + 9 + lfh;
      const int16_t band = (gy + bandH - 10) - gapY;
      const FontID vf = fitValueFont(g, probe, halfW - 28 - unitW, band);
      const int16_t valueFh = (int16_t)g.fontHeight();
      const int16_t baseY = valueBaseline(gapY, band, valueFh);
      setFont(g, vf);
      g.setTextDatum(BL_DATUM);
      g.setTextColor(warn ? dispSettings.warnColor : themeSettings.valueColor);
      g.drawString(text.value, gx + 14, baseY);
      const int16_t vw = (int16_t)g.textWidth(text.value);
      setFont(g, FONT_SMALL);
      g.setTextColor(themeSettings.secondaryColor);
      g.drawString(text.unit, gx + 14 + vw + 5, baseY);
      g.setTextDatum(TL_DATUM);

      if (off) {
        glassBlit(gGlassA, margin, y, halfW, bandH);
        resetFontCache();
      }
    }

    // Right capsule half: the chart, full-bleed inside the rounded shape.
    if (chartTick || fr) {
      GlassCanvas c = off ? glassCanvasFor(gGlassA, &gGlassA, -halfW, 0)
                          : glassCanvasFor(tft, nullptr, margin, y);
      glassPaneWindow(c, halfW, 0, paneW - halfW, bandH, y, paneW, bandH,
                      radius, paneAccent, GLASS_LIQUID, warnRim);
      glassChart(c, pcHistory[vis[b].slotIdx], vis[b].slotIdx,
                 halfW + 4, 8, paneW - halfW - 16, bandH - 18,
                 paneAccent, lineAccent, y, 8, bandH, 10,
                 GLASS_LIQUID, advance, scroll);
      if (off) glassBlit(gGlassA, margin + halfW, y, paneW - halfW, bandH);
    }
  }

  if (n <= 2) return;

  // Remaining metrics as pills: name, reading, and a meter on the floor.
  const int16_t y0 = margin + bands * (bandH + vgap);
  const uint8_t rest = n - 2;
  const uint8_t prows = (rest + 1) / 2;
  const int16_t hgap = 4;
  const int16_t pillW = (w - 2 * margin - hgap) / 2;
  const int16_t pillH = (h - y0 - margin - (prows - 1) * vgap) / prows;
  if (pillH < 16) return;
  const bool poff = ensureSprite(gGlassB, gGlassBW, gGlassBH, pillW, pillH);

  for (uint8_t i = 0; i < rest; i++) {
    const uint8_t vi = i + 2;
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
    snprintf(key, sizeof(key), "%s%s", text.value, warn ? "!" : "");
    if (!gaugeTextChanged(x + 1, y + 1, key, label, fr)) continue;

    lgfx::LovyanGFX& g = poff ? (lgfx::LovyanGFX&)gGlassB : (lgfx::LovyanGFX&)tft;
    const int16_t gx = poff ? 0 : x, gy = poff ? 0 : y;
    if (poff) resetFontCache();
    GlassCanvas c = poff ? glassCanvasFor(gGlassB, &gGlassB, 0, 0)
                         : glassCanvasFor(tft, nullptr, x, y);
    glassPaneWindow(c, 0, 0, pillW, pillH, y, pillW, pillH, 12,
                    paneAccent, GLASS_LIQUID, warnRim);

    // Name and reading share one centreline. Stacking them needs ~34px and a
    // pill is often half that, which is what clipped the labels off the
    // bottom row; the meter then takes the floor on its own.
    const int16_t cy = gy + pillH / 2 - 2;
    setFont(g, FONT_SMALL);
    const int16_t labelW = g.textWidth(label);
    const int16_t unitW = g.textWidth(text.unit);
    g.setTextDatum(ML_DATUM);
    g.setTextColor(glassLabelInk(accent565));
    g.drawString(label, gx + 12, cy);

    char probe[12];
    slotProbe(s, m, probe, sizeof(probe));
    fitValueFont(g, probe, pillW - 34 - labelW - unitW, pillH - 12);
    g.setTextDatum(MR_DATUM);
    g.setTextColor(warn ? dispSettings.warnColor : themeSettings.valueColor);
    g.drawString(text.value, gx + pillW - 11 - unitW - 4, cy);
    setFont(g, FONT_SMALL);
    g.setTextColor(themeSettings.secondaryColor);
    g.drawString(text.unit, gx + pillW - 11, cy);
    g.setTextDatum(TL_DATUM);

    // Meter on the capsule floor, drawn through the compositor so its ends
    // stay soft against the pane instead of clipping to a hard rectangle.
    const int16_t mx = 13, mw = pillW - 26, my = pillH - 6;
    if (mw > 12 && my > 0) {
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
                                  paneAccent, GLASS_LIQUID);
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
}

// Status badge - the LHM trouble indicator now that the bottom bar is gone.
// A small dot in the top-right corner, drawn ONLY while the companion reports
// a non-OK status, so the screen stays clean in the healthy steady state.
// Repainted every frame (it may overlay a cell/tile that redraws itself);
// the status-flip full repaint in drawMonitorStyled clears it residue-free.
static void drawStatusBadge(int16_t w) {
  uint16_t color;
  switch (gStableStatus) {
    case PC_STATUS_API_ERROR:       color = CLR_ORANGE; break;
    case PC_STATUS_LHM_NOT_RUNNING: color = CLR_RED;    break;
    case PC_STATUS_LHM_STARTING:    color = CLR_YELLOW; break;
    default: return;   // OK: no badge
  }
  tft.fillCircle(w - 9, 9, 4, color);
  // Separation ring. On glass the flat bgColor would punch a dark hole in the
  // gradient, so the ring wears the backdrop colour of the row it sits on.
  const uint16_t ring = styleUsesGlass(displayStyle)
    ? rgbTo565(glassSkyAt(9), w - 9, 9) : dispSettings.bgColor;
  tft.drawCircle(w - 9, 9, 5, ring);
}

// Dispatch on the configured monitor face. A live style change (portal save)
// or a companion status flip arrives without a screen-state transition, so
// the previous pixels are still on the panel: detect both here and start
// from a clean screen.
// Compose+blit cost of the last monitor frame, and the worst seen since boot.
// The glass faces run a per-pixel compositor at 20 Hz, so this is the number
// that says whether the frame budget actually holds on a given board.
static uint32_t gFrameUs = 0, gFrameMaxUs = 0;
uint32_t monitorFrameUs()    { return gFrameUs; }
uint32_t monitorFrameMaxUs() { return gFrameMaxUs; }
void resetMonitorFrameStats() { gFrameMaxUs = 0; }

static void drawMonitorStyled(bool fr) {
  const uint32_t t0 = micros();
  static uint8_t lastStyle = 0xFF;
  static uint8_t lastPcStatus = 0xFF;
  const uint8_t stableStatus = debouncedStatus();
  const bool first = (lastStyle == 0xFF);
  if (!gCaptureRender && (displayStyle != lastStyle || stableStatus != lastPcStatus)) {
    if (!first) {
      tft.fillScreen(dispSettings.bgColor);
      resetGaugeTextCache();
      gScreenCleared = true;
      fr = true;
    }
    // Leaving glass frees its compose buffers - the two glass faces size their
    // sprites differently and must not both hold one while only one can draw.
    if (displayStyle != lastStyle && !styleUsesGlass(displayStyle))
      glassReleaseSprites();
    lastStyle = displayStyle;
    lastPcStatus = stableStatus;
  }
  switch (displayStyle) {
    case STYLE_BIG_NUMBERS: drawBigNumbersScreen(fr); break;
    case STYLE_HERO:        drawHeroScreen(fr);       break;
    case STYLE_STRIPS:      drawStripsScreen(fr);     break;
    case STYLE_DUO:         drawDuoScreen(fr);        break;
    case STYLE_PULSE:       drawPulseScreen(fr);      break;
    case STYLE_GLASS_TILES: drawGlassTilesScreen(fr); break;
    case STYLE_GLASS_DUO:   drawGlassDuoScreen(fr);   break;
    case STYLE_TILES:
    default:                drawTilesScreen(fr);      break;
  }
  drawStatusBadge((int16_t)tft.width());
  if (!gCaptureRender) {
    gScreenCleared = false;
    gFrameUs = micros() - t0;
    if (gFrameUs > gFrameMaxUs) gFrameMaxUs = gFrameUs;
  }
}

static void drawIdleClock() {
  switch (clockFace) {
    case CLOCK_FACE_BREAKOUT:
      if (gCaptureRender) drawPongClockSnapshot();
      else tickPongClock();
      break;
    case CLOCK_FACE_MARIO:
      if (gCaptureRender) drawMarioClockSnapshot();
      else tickMarioClock();
      break;
    default:
      if (gCaptureRender) drawClockSnapshot();
      else drawClock();
      break;
  }
}

// ---------------------------------------------------------------------------
//  Frame entry point
// ---------------------------------------------------------------------------
void updateDisplay() {
  unsigned long now = millis();
  bool fr = forceRedraw;

  // Layout edits can move or shrink text and cards, leaving pixels that an
  // opaque value-only repaint does not cover. Clear only on the real panel and
  // only from this display-loop context; capture renders already start with a
  // freshly filled sprite and must not consume a pending panel clear.
  if (clearBeforeRedraw && !gCaptureRender) {
    tft.fillScreen(dispSettings.bgColor);
    resetGaugeTextCache();
    markScreenCleared();
    clearBeforeRedraw = false;
    fr = true;
  }

  // Animated clock faces bypass the monitor renderer's slower update cadence.
  // Screenshot renderers are stateless and never advance the live animation.
  const bool idleVisible = currentScreen == SCREEN_CLOCK ||
                           (currentScreen == SCREEN_MONITOR && !pcData.online);
  const bool animatedClock = clockFace == CLOCK_FACE_BREAKOUT ||
                             clockFace == CLOCK_FACE_MARIO;
  if (idleVisible && animatedClock) {
    if (gCaptureRender) {
      drawIdleClock();
    } else {
      if (currentScreen == SCREEN_MONITOR && prevOnline) {
        tft.fillScreen(dispSettings.bgColor);
        resetGaugeTextCache();
        markScreenCleared();
      }
      drawIdleClock();
      if (currentScreen == SCREEN_MONITOR) prevOnline = false;
    }
    forceRedraw = false;
    return;
  }

  // EVERY monitor face animates its charts now, not just the glass pair, so
  // they all need a frame rate the eye reads as motion rather than the 4 Hz the
  // static screens are paced at. Only while the monitor screen is actually
  // showing a live feed - an idle clock or an offline panel keeps the calm
  // cadence, where there is nothing to animate anyway.
  //
  // Pace off what the LAST frame actually cost so a heavy face can never run
  // back to back and starve WiFi and the web server. Measured on the C3 at
  // 240x240 with 7 bound slots: Big 0.7 ms, Pulse 0.8 ms, Hero 7 ms, Duo 13 ms,
  // Tiles 21 ms, Liquid 46 ms, Aero 96 ms - but Strips 135 ms, because it
  // composes a full-width sprite per lane. A fixed 100 ms would leave that face
  // permanently overrunning. Cheap faces still get the full animation rate; an
  // expensive one degrades to a slower glide instead of eating the loop. This
  // also protects the bigger panels, where every face costs more.
  uint32_t frameInterval = GLASS_UPDATE_MS;
  const uint32_t lastCostMs = gFrameUs / 1000;
  const uint32_t paced = lastCostMs + (lastCostMs >> 1);   // cost + 50% headroom
  if (paced > frameInterval) frameInterval = paced;
  if (frameInterval > 500) frameInterval = 500;
  if (!(currentScreen == SCREEN_MONITOR && pcData.online))
    frameInterval = DISPLAY_UPDATE_MS;
  if (!forceRedraw && (now - lastUpdate < frameInterval)) return;
  lastUpdate = now;

  switch (currentScreen) {
    case SCREEN_SPLASH:           break;  // initDisplay drew it
    case SCREEN_AP_MODE:          if (fr) drawApScreen();        break;
    case SCREEN_CONNECTING_WIFI:  if (fr) drawConnectingScreen(); break;
    case SCREEN_WIFI_CONNECTED:   if (fr) drawConnectedScreen();  break;
    case SCREEN_OTA_UPDATE:       if (fr) drawOtaScreen();        break;
    case SCREEN_MONITOR:
      // Switch to the idle clock when the PC drops offline, back when it returns.
      if (pcData.online) {
        if (!gCaptureRender && (fr || prevOnline != pcData.online)) {
          if (!prevOnline) {
            tft.fillScreen(dispSettings.bgColor);
            resetGaugeTextCache();
            markScreenCleared();
            fr = true;
          }
        }
        drawMonitorStyled(fr);
      } else {
        if (!gCaptureRender && prevOnline) {
          tft.fillScreen(dispSettings.bgColor);
          resetGaugeTextCache();
          markScreenCleared();
          fr = true;
        }
        drawIdleClock();
      }
      // A capture render must not consume an offline->online transition; the
      // panel still needs its physical clear on the next real frame.
      if (!gCaptureRender) prevOnline = pcData.online;
      break;
    case SCREEN_CLOCK:
      drawIdleClock();
      break;
    case SCREEN_OFF:
      break;
  }

  forceRedraw = false;
  markFrameDirty();
}

// Force a repaint on the next updateDisplay() without a screen-state transition.
// Layout-changing callers request a deferred panel clear; capture and live-value
// callers retain the flicker-free opaque repaint path.
void forceDisplayRedraw(bool clearFirst) {
  forceRedraw = true;
  if (clearFirst) clearBeforeRedraw = true;
  lastUpdate = 0;
  resetGaugeTextCache();
}
