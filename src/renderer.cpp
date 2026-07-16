// Renderer: screen-state machine + per-frame drawing for PCMonitorColor.
// Replaces BambuHelper's printer-centric display_ui rendering. Screens:
//   SPLASH/AP/CONNECTING/CONNECTED - status text
//   MONITOR  - gauge grid driven by pcData (auto-mapped for now; configurable
//              metric->gauge assignment lands in the web-portal step)
//   CLOCK    - idle screen shown when the PC is offline
//   OTA_UPDATE - "Updating..." hold screen
#include "display_ui.h"
#include "display_gauges.h"
#include "pc_metrics.h"
#include "settings.h"
#include "fonts.h"
#include "config.h"
#include "layout.h"
#include "wifi_manager.h"
#include <WiFi.h>

static ScreenState currentScreen = SCREEN_SPLASH;
static bool forceRedraw = true;
static unsigned long lastUpdate = 0;
static bool prevOnline = false;

ScreenState getScreenState() { return currentScreen; }

void markScreenCleared();  // defined below with the style helpers

void setScreenState(ScreenState state) {
  if (state == currentScreen) return;
  currentScreen = state;
  forceRedraw = true;
  lastUpdate = 0;            // bypass throttle so the new screen paints at once
  tft.fillScreen(dispSettings.bgColor);
  resetGaugeTextCache();
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

static void drawSlotGauge(const GaugeSlot& slot, const PcMetric& m,
                          const char* label,
                          int16_t cx, int16_t cy, int16_t r, bool fr) {
  uint8_t type = slot.type;
  if (type == GAUGE_TYPE_AUTO || type >= GAUGE_TYPE_COUNT) type = classifyByUnit(m.unit);

  // Accent both arc and label with the slot color; value keeps the default text
  // color (the temp gauge recolors it to warnColor past the warn threshold).
  GaugeColors gc = { slot.arcColor, slot.arcColor, CLR_TEXT };

  switch (type) {
    case GAUGE_TYPE_POWER: {
      float scale = slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.powerScaleW;
      drawPowerGauge(tft, cx, cy, r, m.value, true, label, fr, scale);
      break;
    }
    case GAUGE_TYPE_PERCENT: {
      uint8_t pct = (uint8_t)(m.value < 0 ? 0 : (m.value > 100 ? 100 : m.value));
      drawFanGauge(tft, cx, cy, r, pct, slot.arcColor, label, fr, &gc);
      break;
    }
    case GAUGE_TYPE_FAN: {
      // Generic scaled value (RPM and friends): the temp gauge shows the raw
      // reading in the center while the arc fills 0..scale.
      float scale = slot.scaleMax ? (float)slot.scaleMax : (float)GAUGE_FAN_SCALE_DEFAULT;
      drawTempGauge(tft, cx, cy, r, m.value, 0, scale, slot.arcColor, label, nullptr, fr, &gc);
      break;
    }
    case GAUGE_TYPE_TEMP:
    default: {
      float scale = slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.tempScaleMax;
      drawTempGauge(tft, cx, cy, r, m.value, 0, scale, slot.arcColor, label, nullptr, fr, &gc);
      break;
    }
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
void markScreenCleared() { gScreenCleared = true; }

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
  snprintf(buf, len, "%.0f", slotScaleMax(s, m));
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

// Scale the already-selected VLW font up (2x / 1.5x) when the cell offers the
// room - the biggest bundled face is Inter 19pt, which reads small on a cell
// that holds only a few metrics. Digits have no descenders, so scaled values
// stay inside the box. Leaves the chosen size ACTIVE; the caller must reset
// with tft.setTextSize(1.0f) after drawing the value.
static void scaleValueToCell(const char* s, int16_t maxW, int16_t maxH) {
  static const float scales[] = { 2.0f, 1.5f, 1.0f };
  for (float sc : scales) {
    tft.setTextSize(sc);
    if (tft.fontHeight() <= maxH && tft.textWidth(s) <= maxW) return;
  }
  tft.setTextSize(1.0f);
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

// Sparkline over a slot's history ring, scaled to the local min/max (padded so
// a flat line does not hug an edge), 1px polyline with a bright endpoint dot.
// Composed in a reusable offscreen sprite and pushed in one blit so the
// clear+redraw never flashes at the packet cadence; falls back to direct
// drawing (slightly blinky) only if the sprite cannot be allocated.
static void drawSparkline(const SlotHistory& hist, uint8_t slotIdx,
                          int16_t x, int16_t y,
                          int16_t w, int16_t h, uint16_t color, uint16_t bg,
                          bool advance) {
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
  if (hist.count >= 2) {
    float wlo = pcHistoryAt(hist, 0), whi = wlo;
    for (uint8_t i = 1; i < hist.count; i++) {
      float v = pcHistoryAt(hist, i);
      if (v < wlo) wlo = v;
      if (v > whi) whi = v;
    }
    float pad = (whi - wlo) * 0.15f;
    if (pad < 0.5f) pad = 0.5f;
    wlo -= pad;
    whi += pad;

    // Smoothed per-slot bounds: expand to the window immediately (a new
    // extreme must be visible at once), contract slowly (5% per redraw), so
    // the chart does not rescale-jump every time an extreme leaves the ring.
    static float sLo[NUM_GAUGE_SLOTS], sHi[NUM_GAUGE_SLOTS];
    static uint8_t sInit = 0;
    const uint8_t bit = (uint8_t)(1u << slotIdx);
    float lo, hi;
    if (gCaptureRender) {
      lo = (sInit & bit) ? sLo[slotIdx] : wlo;
      hi = (sInit & bit) ? sHi[slotIdx] : whi;
    } else {
      // Mutate the smoothed bounds only when a new sample arrived - a plain
      // full redraw (portal preview cycle) must reproduce identical pixels,
      // otherwise the chart creeps every 4 s while the preview is open.
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
    }

    // Column-wise render: a dim area fill under a 2px connected line. A bare
    // 1px polyline disappears on the physical panel wherever the series goes
    // flat near the box edge; the filled area keeps the shape readable.
    const uint8_t n = hist.count;
    const int16_t dotR = (h >= 24) ? 2 : 1;
    const int16_t plotW = w - dotR - 1;   // reserve so the endpoint dot stays inside
    const uint16_t areaColor = (uint16_t)((color >> 2) & 0x39E7);  // ~1/4 brightness
    int16_t prevY = 0, sy = 0;
    for (int16_t xi = 0; xi < plotW; xi++) {
      const float fi = (plotW > 1) ? (float)xi * (n - 1) / (plotW - 1) : 0.0f;
      uint8_t i0 = (uint8_t)fi;
      const uint8_t i1 = (uint8_t)((i0 + 1 < n) ? i0 + 1 : n - 1);
      const float t = fi - (float)i0;
      const float v = pcHistoryAt(hist, i0) * (1.0f - t) + pcHistoryAt(hist, i1) * t;
      sy = oy + (h - 1) - (int16_t)((v - lo) * (float)(h - 1) / (hi - lo));
      g.drawFastVLine(ox + xi, sy, oy + h - sy, areaColor);
      const int16_t lineTop = (xi && prevY < sy) ? prevY : sy;
      const int16_t lineBot = (xi && prevY > sy) ? prevY : sy;
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
    g.fillCircle(ox + plotW - 1, dotY, dotR, CLR_TEXT);
  }
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
//  Flicker-free value regions. The old scheme cleared the whole cell/row and
//  repainted it, which blinks at the packet cadence. Instead: opaque glyphs
//  overwrite the previous text in one pass, and only the pixels the new text
//  no longer covers get cleared - painting bg over bg is invisible, so the
//  only pixels that visibly change are the ones that actually changed.
// ---------------------------------------------------------------------------
// Left-aligned value + a small dim unit centered against the value height.
// Caller has already selected the value font (fitFontForWidth +
// scaleValueToCell); resets the text size to 1.0 itself. bandH covers the
// tallest glyph box a previous draw may have used.
static void drawValueRegionL(int16_t x, int16_t baseY, int16_t bandW, int16_t bandH,
                             const char* val, const char* unit,
                             uint16_t fg, uint16_t bg, int16_t* prevVw) {
  const int16_t fh = (int16_t)tft.fontHeight();
  tft.setTextDatum(BL_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(val, x, baseY);
  const int16_t vw = tft.textWidth(val);
  tft.setTextSize(1.0f);
  // The unit and the vacated-pixel clears only need repainting when the
  // value's pixel width changed (digit count change) - doing it every update
  // made the unit blink at the packet rate. prevVw==nullptr forces a full
  // repaint without touching state (capture path).
  if (prevVw && *prevVw == vw) return;
  if (bandH > fh) tft.fillRect(x, baseY - bandH, bandW, bandH - fh, bg);
  if (bandW > vw) tft.fillRect(x + vw, baseY - fh, bandW - vw, fh, bg);
  if (unit && unit[0]) {
    setFont(tft, FONT_SMALL);
    const int16_t unitFh = (int16_t)tft.fontHeight();
    const int16_t unitBaseY = baseY - (fh - unitFh) / 2;
    tft.setTextDatum(BL_DATUM);
    tft.setTextColor(CLR_TEXT_DIM, bg);
    tft.drawString(unit, x + vw + 5, unitBaseY);
  }
  if (prevVw) *prevVw = vw;
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
  tft.setTextColor(CLR_TEXT_DIM, dispSettings.bgColor);
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

    char val[12];
    snprintf(val, sizeof(val), "%.0f", m.value);
    char key[16];
    snprintf(key, sizeof(key), "%s%s", val, warn ? "!" : "");
    if (!gaugeTextChanged(x + cellW / 2, y + cellH / 2, key, label, fr)) continue;

    // No cell clear: every element overwrites itself opaquely, so only the
    // pixels that actually changed get repainted (no blink at packet rate).
    setFont(tft, FONT_SMALL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(CLR_TEXT_DARK, bg);
    const int16_t labelY = y + (roomy ? 8 : 4);
    tft.drawString(label, x + padX, labelY);
    const int16_t lw = tft.textWidth(label);
    tft.fillRect(x + padX + lw, labelY, cellW - 2 * padX - lw, 14, bg);

    // Value (bottom-left, above the meter) + dim unit after it. The value
    // scales up in tall cells, so fewer bound metrics = bigger digits.
    setFont(tft, FONT_SMALL);
    const int16_t unitW = tft.textWidth(m.unit);
    const int16_t baseY = y + cellH - (roomy ? 20 : 12);
    const int16_t bandH = cellH - (roomy ? 44 : 30);
    const int16_t availW = cellW - 2 * padX - unitW - 5;
    char probe[12];
    slotProbe(s, m, probe, sizeof(probe));
    fitFontForWidth(probe, availW, (cellH >= 64) ? FONT_XLARGE : FONT_LARGE);
    scaleValueToCell(probe, availW, bandH);
    static int16_t prevVw[NUM_GAUGE_SLOTS];
    if (fr && !gCaptureRender) prevVw[vis[i].slotIdx] = -1;
    drawValueRegionL(x + padX, baseY, cellW - 2 * padX, bandH, val, m.unit,
                     warn ? dispSettings.warnColor : CLR_TEXT, bg,
                     gCaptureRender ? nullptr : &prevVw[vis[i].slotIdx]);

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
  static uint32_t lastSparkMs = 0;
  bool sparkTick = true;
  if (!gCaptureRender) {
    sparkTick = (pcData.lastReceived != 0) &&
                (millis() - lastSparkMs >= (uint32_t)sparkRedrawSec * 1000UL);
    if (sparkTick) lastSparkMs = millis();
  }

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

    char val[12];
    snprintf(val, sizeof(val), "%.0f", m.value);
    char key[16];
    snprintf(key, sizeof(key), "%s%s", val, warn ? "!" : "");
    const bool head = gaugeTextChanged(x + cardW / 2, y, key, label, fr);

    // Card background only when something actually blanked the area - a
    // plain full redraw (preview capture, style save) repaints content
    // opaquely and refilling the card here would flash it empty first.
    if (fr && (gScreenCleared || relayout || gCaptureRender))
      tft.fillRoundRect(x, y, cardW, cardH, 6, CLR_CARD);

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
        headSpr.fillRoundRect(0, 0, cardW, headH + 8, 6, CLR_CARD);
        if (warn) headSpr.fillRect(0, 6, 3, headH - 6, dispSettings.warnColor);

        const int16_t headCy = headH / 2 + 1;
        setFont(headSpr, FONT_SMALL);
        const int16_t labelW = headSpr.textWidth(label);
        const int16_t unitW  = headSpr.textWidth(m.unit);
        headSpr.setTextDatum(ML_DATUM);
        headSpr.setTextColor(CLR_TEXT_DIM, CLR_CARD);
        headSpr.drawString(label, 9, headCy);
        headSpr.setTextDatum(MR_DATUM);
        headSpr.drawString(m.unit, cardW - 9, headCy);

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
        headSpr.setTextColor(warn ? dispSettings.warnColor : CLR_TEXT, CLR_CARD);
        headSpr.drawString(val, cardW - 9 - unitW - 4, headCy);

        headSpr.pushSprite(tft_ptr, x, y);
        tft.waitDMA();      // same DMA barrier as the spark sprite
        resetFontCache();   // next panel setFont must reload onto the panel
      } else {
        // Sprite unavailable: legacy direct path.
        const int16_t headCy = y + headH / 2 + 1;
        setFont(tft, FONT_SMALL);
        const int16_t labelW = tft.textWidth(label);
        const int16_t unitW  = tft.textWidth(m.unit);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(CLR_TEXT_DIM, CLR_CARD);
        tft.drawString(label, x + 9, headCy);
        tft.setTextDatum(MR_DATUM);
        tft.drawString(m.unit, x + cardW - 9, headCy);
        char probe[12];
        slotProbe(s, m, probe, sizeof(probe));
        fitFontForWidth(probe, cardW - 30 - labelW - unitW,
                        (headH >= 34) ? FONT_LARGE : FONT_BODY);
        drawValueRegionR(x + 9 + labelW + 6, x + cardW - 9 - unitW - 4, headCy,
                         headH - 4, val,
                         warn ? dispSettings.warnColor : CLR_TEXT, CLR_CARD);
      }
      // Warn stripe below the head strip (small overpaint, cannot blink).
      tft.fillRect(x, y + headH, 3, cardH - headH - 6,
                   warn ? dispSettings.warnColor : CLR_CARD);
    }

    if ((sparkTick || fr) && cardH - headH - 10 >= 8) {
      drawSparkline(pcHistory[vis[i].slotIdx], vis[i].slotIdx, x + 8, y + headH + 2,
                    cardW - 16, cardH - headH - 10, lineColor, CLR_CARD, sparkTick);
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

  static uint32_t lastSparkMs = 0;
  bool sparkTick = true;
  if (!gCaptureRender) {
    sparkTick = (pcData.lastReceived != 0) &&
                (millis() - lastSparkMs >= (uint32_t)sparkRedrawSec * 1000UL);
    if (sparkTick) lastSparkMs = millis();
  }

  const int16_t heroH = (gridH * 2) / 5;
  const PcMetric& hm = *vis[0].metric;
  const GaugeSlot& hs = *vis[0].slot;
  const char* heroLabel = vis[0].label;
  const float heroScale = slotScaleMax(hs, hm);
  const bool heroWarn = slotWarn(vis[0].slotIdx, hm.value, heroScale);
  const uint16_t heroColor = hs.arcColor;   // chart keeps identity; value carries warn

  RendererWrite rw(tft);

  char val[12];
  snprintf(val, sizeof(val), "%.0f", hm.value);
  char key[16];
  snprintf(key, sizeof(key), "%s%s", val, heroWarn ? "!" : "");
  // Anchor off-grid (3, heroH) so it can never collide with a row anchor.
  if (gaugeTextChanged(3, heroH, key, heroLabel, fr)) {
    const int16_t heroW = (n >= 2) ? (w / 2) : w;

    setFont(tft, FONT_SMALL);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(CLR_TEXT_DARK, bg);
    tft.drawString(heroLabel, 12, 8);
    const int16_t lw = tft.textWidth(heroLabel);
    tft.fillRect(12 + lw, 8, heroW - 24 - lw, 14, bg);

    setFont(tft, FONT_SMALL);
    const int16_t unitW = tft.textWidth(hm.unit);
    const int16_t baseY = heroH - 12;
    const int16_t bandH = heroH - 36;
    const int16_t availW = heroW - 24 - unitW - 6;
    char probe[12];
    slotProbe(hs, hm, probe, sizeof(probe));
    fitFontForWidth(probe, availW, FONT_XLARGE);
    scaleValueToCell(probe, availW, bandH);
    static int16_t heroPrevVw = -1;
    if (fr && !gCaptureRender) heroPrevVw = -1;
    drawValueRegionL(12, baseY, heroW - 24, bandH, val, hm.unit,
                     heroWarn ? dispSettings.warnColor : CLR_TEXT, bg,
                     gCaptureRender ? nullptr : &heroPrevVw);
  }

  if (fr) tft.drawFastHLine(8, heroH, w - 16, dispSettings.trackColor);

  // Hero sparkline: extend slightly into the center gap while preserving a
  // clear margin after a three-digit hero value and its unit.
  if (sparkTick || fr) {
    if (n >= 2) {
      const int16_t graphX = w / 2 - 10;
      drawSparkline(pcHistory[vis[0].slotIdx], vis[0].slotIdx, graphX, 10,
                    w - graphX - 10, heroH - 20, heroColor, bg, sparkTick);
    } else {
      drawSparkline(pcHistory[vis[0].slotIdx], vis[0].slotIdx, 12, heroH + 8,
                    w - 24, gridH - heroH - 16, heroColor, bg, sparkTick);
    }
  }

  if (n < 2) return;

  // Rows for the remaining metrics. The bar begins closer to the labels and
  // reaches farther right, leaving a stable value area at the panel edge.
  const int16_t rowsY0 = heroH + 2;
  const int16_t rowH = (gridH - rowsY0) / (n - 1);
  const int16_t bx = w / 4;
  const int16_t barRight = (w * 62) / 100;
  const int16_t bw = barRight - bx;
  const int16_t valueLeft = barRight + 3;
  const int16_t valueRight = w - 4;
  const int16_t rowValueW = valueRight - valueLeft;
  int16_t bh = rowH / 5;
  if (bh < 4) bh = 4;
  if (bh > 10) bh = 10;
  const FontID rowLabelFont = (rowH >= 24) ? FONT_BODY : FONT_SMALL;
  for (uint8_t i = 1; i < n; i++) {
    const PcMetric& m = *vis[i].metric;
    const GaugeSlot& s = *vis[i].slot;
    const char* label = vis[i].label;
    const float scale = slotScaleMax(s, m);
    const float frac = slotFraction(m.value, scale);
    const bool warn = slotWarn(vis[i].slotIdx, m.value, scale);
    const int16_t rowY = rowsY0 + (i - 1) * rowH;
    const int16_t cy = rowY + rowH / 2;

    char rv[12];
    snprintf(rv, sizeof(rv), "%.0f", m.value);
    char rkey[16];
    snprintf(rkey, sizeof(rkey), "%s%s", rv, warn ? "!" : "");
    if (!gaugeTextChanged(w / 2, cy, rkey, label, fr)) continue;

    fitFontForWidth(label, bx - 16, rowLabelFont);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(CLR_TEXT_DIM, bg);
    tft.drawString(label, 8, cy);

    drawMeterBar(bx, cy - bh / 2, bw, bh, frac,
                 warn ? dispSettings.warnColor : s.arcColor);

    char vb[20];
    snprintf(vb, sizeof(vb), "%.0f %s", m.value, m.unit);
    char valueProbe[20];
    snprintf(valueProbe, sizeof(valueProbe), "%.0f %s", scale, m.unit);
    fitFontForWidth(valueProbe, rowValueW,
                    (rowH >= 34) ? FONT_LARGE : FONT_BODY);
    drawValueRegionR(valueLeft, valueRight, cy, rowH - 2, vb,
                     warn ? dispSettings.warnColor : CLR_TEXT, bg);
  }
}

static void drawMonitorScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();

  const int16_t gridH = h;

  const int16_t cols = 3;
  const int16_t rows = (NUM_GAUGE_SLOTS + cols - 1) / cols;
  const int16_t cellW = w / cols;
  const int16_t cellH = gridH / rows;

  int16_t r = ((cellW < cellH ? cellW : cellH) / 2) - 8;
  if (r < 12) r = 12;
  if (r > 70) r = 70;   // cap so the value font stays readable on big panels

  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    int16_t cx = (i % cols) * cellW + cellW / 2;
    int16_t cy = (i / cols) * cellH + cellH / 2;

    const GaugeSlot& slot = gaugeMap.slots[i];
    const PcMetric* m = (slot.metricId != 0) ? pcMetricFindById(slot.metricId) : nullptr;
    if (m) {
      drawSlotGauge(slot, *m, gaugeDisplayLabel(i, m->name), cx, cy, r, fr);
    } else if (fr) {
      // Empty/unbound slot: clear it on a full redraw.
      tft.fillCircle(cx, cy, r + 2, dispSettings.bgColor);
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
  tft.drawCircle(w - 9, 9, 5, dispSettings.bgColor);  // separation ring
}

// Dispatch on the configured monitor face. A live style change (portal save)
// or a companion status flip arrives without a screen-state transition, so
// the previous pixels are still on the panel: detect both here and start
// from a clean screen.
static void drawMonitorStyled(bool fr) {
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
    lastStyle = displayStyle;
    lastPcStatus = stableStatus;
  }
  switch (displayStyle) {
    case STYLE_BIG_NUMBERS: drawBigNumbersScreen(fr); break;
    case STYLE_TILES:       drawTilesScreen(fr);      break;
    case STYLE_HERO:        drawHeroScreen(fr);       break;
    default:                drawMonitorScreen(fr);    break;
  }
  drawStatusBadge((int16_t)tft.width());
  if (!gCaptureRender) gScreenCleared = false;
}

static void drawOfflineScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  if (fr) {
    tft.setTextDatum(MC_DATUM);
    setFont(tft, FONT_BODY);
    tft.setTextColor(CLR_TEXT_DIM, dispSettings.bgColor);
    tft.drawString("PC offline", w / 2, h / 2 - 60);
  }
  // Reuse the gauge clock widget as the idle face.
  drawClockWidget(tft, w / 2, h / 2 + 6, 44, 6, fr);
}

// ---------------------------------------------------------------------------
//  Frame entry point
// ---------------------------------------------------------------------------
void updateDisplay() {
  unsigned long now = millis();
  if (!forceRedraw && (now - lastUpdate < DISPLAY_UPDATE_MS)) return;
  lastUpdate = now;
  bool fr = forceRedraw;

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
            gScreenCleared = true;
            fr = true;
          }
        }
        drawMonitorStyled(fr);
      } else {
        if (!gCaptureRender && (fr || prevOnline)) {
          tft.fillScreen(dispSettings.bgColor); resetGaugeTextCache(); fr = true;
        }
        drawOfflineScreen(fr);
      }
      // A capture render must not consume an offline->online transition; the
      // panel still needs its physical clear on the next real frame.
      if (!gCaptureRender) prevOnline = pcData.online;
      break;
    case SCREEN_CLOCK:
      drawOfflineScreen(fr);
      break;
    case SCREEN_OFF:
      break;
  }

  forceRedraw = false;
  markFrameDirty();
}

// Force a clean full repaint on the next updateDisplay() without a screen-state
// transition. Used after a live config change (e.g. saving the gauge mapping)
// so the new layout/colors take effect without a reboot. Touches no pixels here
// (the draw happens in the loop's updateDisplay), so it is safe to call from the
// web-server handler.
void forceDisplayRedraw() {
  forceRedraw = true;
  lastUpdate = 0;
  resetGaugeTextCache();
}
