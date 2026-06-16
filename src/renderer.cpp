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

void setScreenState(ScreenState state) {
  if (state == currentScreen) return;
  currentScreen = state;
  forceRedraw = true;
  lastUpdate = 0;            // bypass throttle so the new screen paints at once
  tft.fillScreen(dispSettings.bgColor);
  resetGaugeTextCache();
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
                          int16_t cx, int16_t cy, int16_t r, bool fr) {
  uint8_t type = slot.type;
  if (type == GAUGE_TYPE_AUTO || type >= GAUGE_TYPE_COUNT) type = classifyByUnit(m.unit);

  // Accent both arc and label with the slot color; value keeps the default text
  // color (the temp gauge recolors it to warnColor past the warn threshold).
  GaugeColors gc = { slot.arcColor, slot.arcColor, CLR_TEXT };

  switch (type) {
    case GAUGE_TYPE_POWER: {
      float scale = slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.powerScaleW;
      drawPowerGauge(tft, cx, cy, r, m.value, true, m.name, fr, scale);
      break;
    }
    case GAUGE_TYPE_PERCENT: {
      uint8_t pct = (uint8_t)(m.value < 0 ? 0 : (m.value > 100 ? 100 : m.value));
      drawFanGauge(tft, cx, cy, r, pct, slot.arcColor, m.name, fr, &gc);
      break;
    }
    case GAUGE_TYPE_FAN: {
      // Generic scaled value (RPM and friends): the temp gauge shows the raw
      // reading in the center while the arc fills 0..scale.
      float scale = slot.scaleMax ? (float)slot.scaleMax : (float)GAUGE_FAN_SCALE_DEFAULT;
      drawTempGauge(tft, cx, cy, r, m.value, 0, scale, slot.arcColor, m.name, nullptr, fr, &gc);
      break;
    }
    case GAUGE_TYPE_TEMP:
    default: {
      float scale = slot.scaleMax ? (float)slot.scaleMax : (float)dispSettings.tempScaleMax;
      drawTempGauge(tft, cx, cy, r, m.value, 0, scale, slot.arcColor, m.name, nullptr, fr, &gc);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
//  Bottom info bar - the two facts the companion's stream actually carries:
//  the LHM health status (as a colored dot + short label) and the PC's
//  last-update time (packet timestamp). The protocol has no uptime/hostname
//  field, so nothing else PC-side is shown here. Redraws only when content
//  changes, so it does not flicker at the frame rate.
// ---------------------------------------------------------------------------
static void pcStatusLabel(uint8_t status, const char*& text, uint16_t& color) {
  switch (status) {
    case PC_STATUS_OK:              text = "OK";      color = CLR_GREEN;    break;
    case PC_STATUS_API_ERROR:       text = "API err"; color = CLR_ORANGE;   break;
    case PC_STATUS_LHM_NOT_RUNNING: text = "LHM off"; color = CLR_RED;      break;
    case PC_STATUS_LHM_STARTING:    text = "LHM...";  color = CLR_YELLOW;   break;
    default:                        text = "?";       color = CLR_TEXT_DIM; break;
  }
}

static void drawBottomBar(int16_t barY, int16_t barH, int16_t w, bool fr) {
  const char* statusText;
  uint16_t dotColor;
  pcStatusLabel(pcData.status, statusText, dotColor);
  const char* ts = pcData.timestamp[0] ? pcData.timestamp : "--:--";

  static uint8_t lastStatus = 0xFF;
  static char    lastTs[6]  = "";
  if (!fr && pcData.status == lastStatus && strcmp(ts, lastTs) == 0) return;
  lastStatus = pcData.status;
  strlcpy(lastTs, ts, sizeof(lastTs));

  uint16_t bg = dispSettings.bgColor;
  const int16_t cy = barY + barH / 2;

  tft.startWrite();
  tft.fillRect(0, barY, w, barH, bg);
  tft.drawFastHLine(0, barY, w, dispSettings.trackColor);   // divider above bar

  // Left: status dot + label.
  const int16_t dotR = 4;
  const int16_t dotX = 8 + dotR;
  tft.fillCircle(dotX, cy, dotR, dotColor);
  setFont(tft, FONT_SMALL);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(CLR_TEXT_DIM, bg);
  tft.drawString(statusText, dotX + dotR + 6, cy);

  // Right: PC last-update time.
  tft.setTextDatum(MR_DATUM);
  tft.setTextColor(CLR_TEXT, bg);
  tft.drawString(ts, w - 8, cy);
  tft.endWrite();
}

static void drawMonitorScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();

  // Reserve a slim strip at the bottom for the info bar; lay the gauge grid out
  // in the remaining height so labels never collide with the bar.
  const int16_t barH  = (h / 10 > 24) ? 24 : (int16_t)(h / 10);
  const int16_t gridH = h - barH;

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
      drawSlotGauge(slot, *m, cx, cy, r, fr);
    } else if (fr) {
      // Empty/unbound slot: clear it on a full redraw.
      tft.fillCircle(cx, cy, r + 2, dispSettings.bgColor);
    }
  }

  drawBottomBar(gridH, barH, w, fr);
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
        if (fr || prevOnline != pcData.online) {
          if (!prevOnline) { tft.fillScreen(dispSettings.bgColor); resetGaugeTextCache(); fr = true; }
        }
        drawMonitorScreen(fr);
      } else {
        if (fr || prevOnline) {
          tft.fillScreen(dispSettings.bgColor); resetGaugeTextCache(); fr = true;
        }
        drawOfflineScreen(fr);
      }
      prevOnline = pcData.online;
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
