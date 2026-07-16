// PCMonitorColor - firmware entry point.
// Boot: settings -> display -> WiFi (STA or AP setup) -> web/OTA server -> UDP
// metric listener. The device is a passive receiver: the PC companion pushes
// V2.1 JSON to UDP :4210, which drives the gauge grid. When no packets arrive
// the renderer falls back to the idle clock.
#include <Arduino.h>
#include "config.h"
#include "display_ui.h"
#include "settings.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "pc_metrics.h"
#include "touch_button.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(PRODUCT_NAME " " FW_VERSION " boot");

  loadSettings();
  initDisplay();                 // draws the splash
  setScreenState(SCREEN_SPLASH);

  startWiFiDuringSplash();        // kick off a background STA connect
  unsigned long splashStart = millis();
  while (millis() - splashStart < 800) {
    updateDisplay();
    flushFrame();
    delay(20);
  }

  initWiFi();                     // blocks until STA connects or AP comes up
  initWebServer();
  pcMetricsBegin();
  initTouchButton();

  if (isWiFiConnected()) setScreenState(SCREEN_MONITOR);
}

void loop() {
  handleWiFi();
  handleWebServer();
  pcMetricsHandle();
  handleTouchButton();

  // Finish boot/connect transitions without overriding a touch-selected clock.
  ScreenState s = getScreenState();
  if (isWiFiConnected() &&
      (s == SCREEN_SPLASH || s == SCREEN_CONNECTING_WIFI ||
       s == SCREEN_WIFI_CONNECTED)) {
    setScreenState(SCREEN_MONITOR);
  }

  updateBacklightControl();
  updateDisplay();
  flushFrame();
}
