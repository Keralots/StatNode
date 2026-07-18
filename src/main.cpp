// StatNode - firmware entry point.
// Boot: settings -> display -> WiFi (STA or AP setup) -> web/OTA server -> UDP
// metric listener. The device is a passive receiver: the PC companion pushes
// V2.1 JSON to UDP :4210, which drives the gauge grid. When no packets arrive
// the renderer falls back to the idle clock.
#include <Arduino.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "display_ui.h"
#include "settings.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "pc_metrics.h"
#include "touch_button.h"

// Loop-task watchdog. The Arduino core leaves the idle-task watchdog disabled,
// so any indefinite block in loop() used to hang the device silently and
// forever (frozen panel, dead web server, no reset). With the loop task
// subscribed, such a block now panics after WDT_TIMEOUT_S with a backtrace on
// serial and the device reboots itself; /api/status then reports
// reset_reason "task_wdt"/"panic". Every legitimate blocking path (OTA flash
// writes, NVS commits, chunked asset streaming, the 4 s-capped client writes)
// finishes far below this bound.
static const uint32_t WDT_TIMEOUT_S = 30;

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(PRODUCT_NAME " " FW_VERSION " boot");
  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());

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

  // Subscribe only after the blocking boot phases (WiFi connect windows, IP
  // splash) are done; loop() feeds it every pass. The Arduino core usually
  // arrives with the TWDT already initialized (watching nothing), so fall
  // back to reconfigure when init reports that state.
  const esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_init(&wdtConfig) != ESP_OK)
    esp_task_wdt_reconfigure(&wdtConfig);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
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
