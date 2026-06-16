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
//  Monitor screen - auto-map the first metrics to a 2x2 gauge grid.
//  Classification by unit; real configurable mapping is the web-portal step.
// ---------------------------------------------------------------------------
static void drawOneMetricGauge(const PcMetric& m, int16_t cx, int16_t cy,
                               int16_t r, bool fr) {
  const GaugeColors* gc = &dispSettings.gauge;
  char unit0 = m.unit[0];

  if (unit0 == 'C') {
    drawTempGauge(tft, cx, cy, r, m.value, 0, (float)dispSettings.tempScaleMax,
                  CLR_ORANGE, m.name, nullptr, fr, gc);
  } else if (unit0 == 'W') {
    drawPowerGauge(tft, cx, cy, r, m.value, true, m.name, fr);
  } else if (unit0 == '%') {
    uint8_t pct = (uint8_t)(m.value < 0 ? 0 : (m.value > 100 ? 100 : m.value));
    drawFanGauge(tft, cx, cy, r, pct, CLR_GREEN, m.name, fr, gc);
  } else {
    // Unknown unit (RPM, MB, ...): show as a fan-style numeric gauge scaled to
    // the value's own magnitude so the arc is still informative.
    uint8_t pct = (uint8_t)(m.value < 0 ? 0 : (m.value > 100 ? 100 : m.value));
    drawFanGauge(tft, cx, cy, r, pct, CLR_BLUE, m.name, fr, gc);
  }
}

static void drawMonitorScreen(bool fr) {
  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t r = LY_GAUGE_R;
  const int16_t cx[4] = { (int16_t)(w / 4), (int16_t)(w - w / 4),
                          (int16_t)(w / 4), (int16_t)(w - w / 4) };
  const int16_t cy[4] = { (int16_t)(h / 3), (int16_t)(h / 3),
                          (int16_t)(h - h / 3), (int16_t)(h - h / 3) };

  uint8_t n = pcData.count;
  if (n > 4) n = 4;
  for (uint8_t i = 0; i < 4; i++) {
    if (i < n) {
      drawOneMetricGauge(pcData.metrics[i], cx[i], cy[i], r, fr);
    } else if (fr) {
      // Clear unused slots on a full redraw.
      tft.fillCircle(cx[i], cy[i], r + 2, dispSettings.bgColor);
    }
  }
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
