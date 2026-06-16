// PCMonitorColor - hello-gauges bring-up.
// First milestone: prove the ported display chassis compiles and renders arc
// gauges. Values are dummy oscillators here; the UDP ingest + metric->gauge
// mapping land in later steps (see pc_metrics.* and the renderer).
#include <Arduino.h>
#include <math.h>
#include "config.h"
#include "layout.h"
#include "display_ui.h"
#include "display_gauges.h"
#include "settings.h"
#include "fonts.h"

static bool firstFrame = true;
static unsigned long lastUpdate = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(PRODUCT_NAME " " FW_VERSION " boot");

  loadSettings();
  initDisplay();
}

void loop() {
  unsigned long now = millis();
  if (now - lastUpdate < DISPLAY_UPDATE_MS) {
    flushFrame();
    return;
  }
  lastUpdate = now;

  // Dummy animated values until the UDP metric source is wired in.
  float t = now / 1000.0f;
  float cpuTemp = 45.0f + 35.0f * (0.5f + 0.5f * sinf(t * 0.70f));
  float gpuTemp = 50.0f + 30.0f * (0.5f + 0.5f * sinf(t * 0.50f + 1.0f));
  uint8_t cpuLoad = (uint8_t)(50.0f + 50.0f * sinf(t * 0.90f));
  uint8_t gpuLoad = (uint8_t)(50.0f + 50.0f * sinf(t * 1.10f + 2.0f));

  const int16_t w = (int16_t)tft.width();
  const int16_t h = (int16_t)tft.height();
  const int16_t r = LY_GAUGE_R;
  const int16_t cx1 = w / 4;
  const int16_t cx2 = w - w / 4;
  const int16_t cy1 = h / 3;
  const int16_t cy2 = h - h / 3;
  const bool fr = firstFrame;

  drawTempGauge(tft, cx1, cy1, r, cpuTemp, 0, (float)dispSettings.tempScaleMax,
                CLR_ORANGE, "CPU", nullptr, fr, &dispSettings.gauge);
  drawTempGauge(tft, cx2, cy1, r, gpuTemp, 0, (float)dispSettings.tempScaleMax,
                CLR_CYAN, "GPU", nullptr, fr, &dispSettings.gauge);
  drawFanGauge(tft, cx1, cy2, r, cpuLoad, CLR_GREEN, "LOAD", fr, &dispSettings.gauge);
  drawFanGauge(tft, cx2, cy2, r, gpuLoad, CLR_BLUE, "GPU%", fr, &dispSettings.gauge);

  firstFrame = false;
  flushFrame();
}
