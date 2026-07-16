// PCMonitorColor web portal, JSON APIs, live screen capture, OTA update, and
// captive-portal redirects. Static UI assets are streamed from PROGMEM in
// bounded chunks so low-RAM boards never assemble the full page in heap.
#include "web_server.h"
#include "settings.h"
#include "wifi_manager.h"
#include "display_ui.h"
#include "pc_metrics.h"
#include "fonts.h"
#include "config.h"
#include "web_pages.h"
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <WiFi.h>
#include <time.h>

static WebServer server(80);

// Deferred restart (so the HTTP response flushes before reboot).
static unsigned long restartAt = 0;
static void scheduleRestart(unsigned long delayMs) { restartAt = millis() + delayMs; }

// OTA state.
static bool   otaInProgress = false;
static bool   otaFirstChunk = true;
static String otaError = "";

bool        isOtaAutoInProgress() { return otaInProgress; }
int         getOtaAutoProgress()  { return 0; }
const char* getOtaAutoStatus()    { return otaError.c_str(); }

// ---------------------------------------------------------------------------
//  Portal assets and JSON helpers
// ---------------------------------------------------------------------------
static bool writeClientAll(WiFiClient& client, const uint8_t* data, size_t len) {
  unsigned long lastProgress = millis();
  while (len > 0 && client.connected()) {
    size_t sent = client.write(data, len);
    if (sent > 0) {
      data += sent;
      len -= sent;
      lastProgress = millis();
    } else {
      if (millis() - lastProgress > 4000) return false;
      delay(1);
    }
  }
  return len == 0;
}

static void streamAsset(const char* data, size_t len, const char* contentType,
                        const char* cacheControl) {
  static const size_t CHUNK_SIZE = 2048;
  uint8_t* chunk = (uint8_t*)malloc(CHUNK_SIZE);
  if (!chunk) {
    server.send(503, "text/plain", "Out of memory");
    return;
  }

  server.sendHeader("Cache-Control", cacheControl);
  server.setContentLength(len);
  server.send(200, contentType, "");
  WiFiClient client = server.client();

  bool ok = true;
  for (size_t offset = 0; offset < len && ok; offset += CHUNK_SIZE) {
    size_t count = min(CHUNK_SIZE, len - offset);
    memcpy_P(chunk, data + offset, count);
    ok = writeClientAll(client, chunk, count);
    delay(0);
  }
  free(chunk);
  if (!ok) client.stop();
}

static void handleRoot() {
  streamAsset(PAGE_HTML, sizeof(PAGE_HTML) - 1, "text/html",
              "no-cache, no-store, must-revalidate");
}

static void handlePortalCss() {
  streamAsset(PORTAL_CSS, sizeof(PORTAL_CSS) - 1, "text/css",
              "public, max-age=31536000, immutable");
}

static void handlePortalLayoutCss() {
  streamAsset(PORTAL_LAYOUT_CSS, sizeof(PORTAL_LAYOUT_CSS) - 1, "text/css",
              "public, max-age=31536000, immutable");
}

static void handlePortalJs() {
  streamAsset(PORTAL_JS, sizeof(PORTAL_JS) - 1, "application/javascript",
              "public, max-age=31536000, immutable");
}

static void sendJsonMessage(int status, bool ok, const char* message,
                            bool restarting = false) {
  JsonDocument doc;
  doc["success"] = ok;
  doc["message"] = message;
  if (restarting) doc["restarting"] = true;
  String out;
  serializeJson(doc, out);
  server.send(status, "application/json", out);
}

static long clampedArg(const char* name, long current, long minValue, long maxValue) {
  if (!server.hasArg(name)) return current;
  long value = server.arg(name).toInt();
  if (value < minValue) value = minValue;
  if (value > maxValue) value = maxValue;
  return value;
}

static void addHtmlColor(JsonObject object, const char* key, uint16_t color) {
  char buf[8];
  rgb565ToHtml(color, buf);
  object[key] = buf;
}

static void formatMinuteOfDay(uint16_t minute, char* out, size_t outSize) {
  snprintf(out, outSize, "%02u:%02u", minute / 60, minute % 60);
}

static uint16_t minuteOfDayArg(const char* name, uint16_t current) {
  if (!server.hasArg(name)) return current;
  int hour = -1, minute = -1;
  if (sscanf(server.arg(name).c_str(), "%d:%d", &hour, &minute) != 2)
    return current;
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return current;
  return (uint16_t)(hour * 60 + minute);
}

static void handleApiConfig() {
  JsonDocument doc;
  doc["product"] = PRODUCT_NAME;
  doc["version"] = FW_VERSION;
  doc["board"] = BOARD_VARIANT;
  doc["ip"] = WiFi.localIP().toString();
#if defined(DISPLAY_CYD)
  doc["isCyd"] = true;
#else
  doc["isCyd"] = false;
#endif

  JsonObject display = doc["display"].to<JsonObject>();
  display["style"] = displayStyle;
  display["rotation"] = dispSettings.rotation;
  display["brightness"] = brightness;
  display["smoothing"] = dispSettings.gaugeSmoothing;
  display["sparkSeconds"] = sparkRedrawSec;
  display["tempScale"] = dispSettings.tempScaleMax;
  display["powerScale"] = dispSettings.powerScaleW;
  display["warnThreshold"] = dispSettings.warnThresholdPct;
  display["smallLabels"] = dispSettings.smallLabels;
  display["invertColors"] = dispSettings.invertColors;
  display["cydClassic"] = dispSettings.cydPanelClassic;

  JsonObject backlight = doc["backlight"].to<JsonObject>();
  backlight["nightEnabled"] = backlightSettings.nightEnabled != 0;
  backlight["nightBrightness"] = backlightSettings.nightBrightness;
  char timeBuf[6];
  formatMinuteOfDay(backlightSettings.nightStartMinute, timeBuf, sizeof(timeBuf));
  backlight["nightStart"] = timeBuf;
  formatMinuteOfDay(backlightSettings.nightEndMinute, timeBuf, sizeof(timeBuf));
  backlight["nightEnd"] = timeBuf;
  backlight["offlineSleepMinutes"] = backlightSettings.offlineSleepMinutes;

  JsonObject colors = doc["colors"].to<JsonObject>();
  addHtmlColor(colors, "bg", dispSettings.bgColor);
  addHtmlColor(colors, "track", dispSettings.trackColor);
  addHtmlColor(colors, "warn", dispSettings.warnColor);
  addHtmlColor(colors, "clock", dispSettings.clockTimeColor);
  addHtmlColor(colors, "date", dispSettings.clockDateColor);

  JsonObject clock = doc["clock"].to<JsonObject>();
  clock["use24h"] = netSettings.use24h;
  clock["timezone"] = netSettings.timezoneStr;

  JsonObject network = doc["network"].to<JsonObject>();
  network["ssid"] = wifiSSID;
  network["hostname"] = netSettings.hostname;
  network["mdns"] = netSettings.mdnsEnabled;
  network["showIp"] = netSettings.showIPAtStartup;
  network["dhcp"] = netSettings.useDHCP;
  network["staticIp"] = netSettings.staticIP;
  network["gateway"] = netSettings.gateway;
  network["subnet"] = netSettings.subnet;
  network["dns"] = netSettings.dns;

  JsonArray gauges = doc["gauges"].to<JsonArray>();
  char colorBuf[8];
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    JsonObject slot = gauges.add<JsonObject>();
    slot["metricId"] = gaugeMap.slots[i].metricId;
    slot["type"] = gaugeMap.slots[i].type;
    slot["scaleMax"] = gaugeMap.slots[i].scaleMax;
    slot["label"] = gaugeLabels.labels[i];
    rgb565ToHtml(gaugeMap.slots[i].arcColor, colorBuf);
    slot["color"] = colorBuf;
  }

  JsonArray metrics = doc["metrics"].to<JsonArray>();
  for (uint8_t i = 0; i < pcData.count; i++) {
    JsonObject metric = metrics.add<JsonObject>();
    metric["id"] = pcData.metrics[i].id;
    metric["name"] = pcData.metrics[i].name;
    metric["value"] = pcData.metrics[i].value;
    metric["unit"] = pcData.metrics[i].unit;
  }

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
//  Save handlers
// ---------------------------------------------------------------------------
static void handleSaveDisplay() {
  bool panelChanged = false;
  displayStyle = (uint8_t)clampedArg("style", displayStyle, 0, STYLE_COUNT - 1);
  dispSettings.rotation = (uint8_t)clampedArg("rotation", dispSettings.rotation, 0, 3);
  brightness = (uint8_t)clampedArg("brightness", brightness, 0, 255);
  dispSettings.gaugeSmoothing = (uint8_t)clampedArg("smoothing", dispSettings.gaugeSmoothing, 0, 3);
  sparkRedrawSec = (uint8_t)clampedArg("sparks", sparkRedrawSec, 1, 60);
  dispSettings.tempScaleMax = (uint16_t)clampedArg("tempScale", dispSettings.tempScaleMax, 1, 500);
  dispSettings.powerScaleW = (uint16_t)clampedArg("powerScale", dispSettings.powerScaleW, 1, 65535);
  dispSettings.warnThresholdPct = (uint8_t)clampedArg("warnThreshold", dispSettings.warnThresholdPct, 0, 100);
  dispSettings.smallLabels = server.hasArg("smallLabels");
  dispSettings.invertColors = server.hasArg("invertColors");
  backlightSettings.nightEnabled = server.hasArg("nightEnabled") ? 1 : 0;
  backlightSettings.nightBrightness =
    (uint8_t)clampedArg("nightBrightness", backlightSettings.nightBrightness, 0, 255);
  backlightSettings.nightStartMinute =
    minuteOfDayArg("nightStart", backlightSettings.nightStartMinute);
  backlightSettings.nightEndMinute =
    minuteOfDayArg("nightEnd", backlightSettings.nightEndMinute);
  backlightSettings.offlineSleepMinutes =
    (uint16_t)clampedArg("offlineSleep", backlightSettings.offlineSleepMinutes, 0, 1440);
#if defined(DISPLAY_CYD)
  bool cydClassic = server.hasArg("cydClassic");
  panelChanged = cydClassic != dispSettings.cydPanelClassic;
  dispSettings.cydPanelClassic = cydClassic;
#endif

  saveSettings();
  refreshBacklightControl();
  applyDisplaySettings();
  markScreenCleared();
  forceDisplayRedraw();
  sendJsonMessage(200, true,
                  panelChanged ? "Display saved. Restarting for the panel change."
                               : "Display settings applied.",
                  panelChanged);
  if (panelChanged) scheduleRestart(1500);
}

static void handleSaveGauges() {
  // Keep style and chart cadence accepted here for backwards compatibility
  // with existing scripts that posted them to /save/gauges.
  if (server.hasArg("style"))
    displayStyle = (uint8_t)clampedArg("style", displayStyle, 0, STYLE_COUNT - 1);
  if (server.hasArg("sparks"))
    sparkRedrawSec = (uint8_t)clampedArg("sparks", sparkRedrawSec, 1, 60);

  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    GaugeSlot& slot = gaugeMap.slots[i];
    char key[4];
    snprintf(key, sizeof(key), "m%u", i);
    slot.metricId = (uint8_t)clampedArg(key, slot.metricId, 0, MAX_METRICS);
    snprintf(key, sizeof(key), "t%u", i);
    slot.type = (uint8_t)clampedArg(key, slot.type, 0, GAUGE_TYPE_COUNT - 1);
    snprintf(key, sizeof(key), "s%u", i);
    slot.scaleMax = (uint16_t)clampedArg(key, slot.scaleMax, 0, 65535);
    snprintf(key, sizeof(key), "c%u", i);
    if (server.hasArg(key)) slot.arcColor = htmlToRgb565(server.arg(key).c_str());
    snprintf(key, sizeof(key), "l%u", i);
    if (server.hasArg(key)) {
      sanitizeGaugeLabel(server.arg(key).c_str(), gaugeLabels.labels[i],
                         sizeof(gaugeLabels.labels[i]));
    }
  }
  saveSettings();
  forceDisplayRedraw();
  sendJsonMessage(200, true, "Metric layout applied.");
}

struct ColorPreset {
  const char* id;
  uint16_t bg, track, warn, clockTime, clockDate;
  uint16_t slots[NUM_GAUGE_SLOTS];
};

static const ColorPreset kPresets[] = {
  // Factory defaults (config.h palette).
  { "default", CLR_BG, CLR_TRACK, CLR_RED, CLR_TEXT, CLR_TEXT_DIM,
    { CLR_ORANGE, CLR_BLUE, CLR_GREEN, CLR_CYAN, CLR_GOLD, CLR_RED } },
  // "Modern": the colorblind-validated categorical set from the redesign.
  { "modern", CLR_BG, 0x2151 /*#202830*/, 0xE249 /*#E5484D*/, CLR_TEXT, CLR_TEXT_DIM,
    { 0x3C3C /*#3987E5*/, 0x0400 /*#008300*/, 0xD290 /*#D55181*/,
      0xCC20 /*#C98500*/, 0x1CEE /*#199E70*/, 0xDAC4 /*#D95926*/ } },
};

static void handleSaveColors() {
  const String pre = server.arg("preset");
  const ColorPreset* preset = nullptr;
  for (const ColorPreset& cp : kPresets) {
    if (pre == cp.id) { preset = &cp; break; }
  }
  if (preset) {
    dispSettings.bgColor        = preset->bg;
    dispSettings.trackColor     = preset->track;
    dispSettings.warnColor      = preset->warn;
    dispSettings.clockTimeColor = preset->clockTime;
    dispSettings.clockDateColor = preset->clockDate;
    for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++)
      gaugeMap.slots[i].arcColor = preset->slots[i];
  } else {
    if (server.hasArg("cbg"))    dispSettings.bgColor        = htmlToRgb565(server.arg("cbg").c_str());
    if (server.hasArg("ctrack")) dispSettings.trackColor     = htmlToRgb565(server.arg("ctrack").c_str());
    if (server.hasArg("cwarn"))  dispSettings.warnColor      = htmlToRgb565(server.arg("cwarn").c_str());
    if (server.hasArg("cct"))    dispSettings.clockTimeColor = htmlToRgb565(server.arg("cct").c_str());
    if (server.hasArg("ccd"))    dispSettings.clockDateColor = htmlToRgb565(server.arg("ccd").c_str());
  }
  saveSettings();
  applyDisplaySettings();   // repaints the panel with the new background
  markScreenCleared();      // styles must repaint their static chrome
  forceDisplayRedraw();
  sendJsonMessage(200, true, "Color palette applied.");
}

static void handleSaveClock() {
  netSettings.use24h = server.arg("timeFormat") != "12";
  if (server.hasArg("timezone"))
    strlcpy(netSettings.timezoneStr, server.arg("timezone").c_str(),
            sizeof(netSettings.timezoneStr));
  saveSettings();
  configTzTime(netSettings.timezoneStr, "pool.ntp.org", "time.nist.gov");
  refreshBacklightControl();
  forceDisplayRedraw();
  sendJsonMessage(200, true, "Clock settings applied.");
}

static bool validIp(const String& value, bool allowEmpty = false) {
  if (allowEmpty && value.length() == 0) return true;
  IPAddress parsed;
  return parsed.fromString(value);
}

static void handleSaveNetwork() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32) {
    sendJsonMessage(400, false, "WiFi network name must be 1 to 32 characters.");
    return;
  }

  NetworkSettings next = netSettings;
  next.useDHCP = server.arg("ipMode") != "static";
  next.mdnsEnabled = server.hasArg("mdns");
  next.showIPAtStartup = server.hasArg("showIp");
  if (server.hasArg("hostname"))
    sanitizeHostname(server.arg("hostname").c_str(), next.hostname, sizeof(next.hostname));

  if (!next.useDHCP) {
    String ip = server.arg("staticIp"); ip.trim();
    String gateway = server.arg("gateway"); gateway.trim();
    String subnet = server.arg("subnet"); subnet.trim();
    String dns = server.arg("dns"); dns.trim();
    if (!validIp(ip) || !validIp(gateway) || !validIp(subnet) || !validIp(dns, true)) {
      sendJsonMessage(400, false, "Enter valid IPv4 values for the static network configuration.");
      return;
    }
    strlcpy(next.staticIP, ip.c_str(), sizeof(next.staticIP));
    strlcpy(next.gateway, gateway.c_str(), sizeof(next.gateway));
    strlcpy(next.subnet, subnet.c_str(), sizeof(next.subnet));
    strlcpy(next.dns, dns.c_str(), sizeof(next.dns));
  }

  strlcpy(wifiSSID, ssid.c_str(), sizeof(wifiSSID));
  if (server.hasArg("pass") && server.arg("pass").length() > 0)
    strlcpy(wifiPass, server.arg("pass").c_str(), sizeof(wifiPass));
  netSettings = next;
  saveSettings();
  sendJsonMessage(200, true, "Network settings saved. Device is restarting.", true);
  scheduleRestart(1500);
}

static void handleSaveWifiLegacy() {
  String ssid = server.arg("ssid");
  ssid.trim();
  if (ssid.length() == 0 || ssid.length() > 32) {
    sendJsonMessage(400, false, "WiFi network name must be 1 to 32 characters.");
    return;
  }
  strlcpy(wifiSSID, ssid.c_str(), sizeof(wifiSSID));
  if (server.hasArg("pass") && server.arg("pass").length() > 0)
    strlcpy(wifiPass, server.arg("pass").c_str(), sizeof(wifiPass));
  saveSettings();
  sendJsonMessage(200, true, "WiFi settings saved. Device is restarting.", true);
  scheduleRestart(1500);
}

static void handleApiStatus() {
  JsonDocument doc;
  doc["product"] = PRODUCT_NAME;
  doc["version"] = FW_VERSION;
  doc["board"]   = BOARD_VARIANT;
  doc["ip"]      = WiFi.localIP().toString();
  doc["pc_online"] = pcData.online;
  doc["pc_status"] = pcData.status;
  doc["style"]     = displayStyle;
  doc["backlight"] = currentBacklightLevel();
  doc["night_active"] = nightBrightnessActive();
  doc["offline_sleeping"] = offlineDisplaySleeping();
  doc["time_valid"] = backlightTimeValid();
  doc["spark_sprite"] = sparkSpriteActive();
  doc["spark_fails"]  = sparkSpriteFails();
  doc["raw_n_changes"]      = rawNChanges();
  doc["raw_status_changes"] = rawStatusChanges();
  doc["relayouts"]          = acceptedRelayouts();
  doc["status_flips"]       = acceptedStatusFlips();
  doc["free_heap"]    = ESP.getFreeHeap();
  doc["max_block"]    = ESP.getMaxAllocHeap();
  doc["metric_count"] = pcData.count;
  doc["timestamp"] = pcData.timestamp;
  doc["hostname"] = netSettings.hostname;
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["uptime_seconds"] = millis() / 1000UL;
  doc["ap_mode"] = isAPMode();
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

// ---------------------------------------------------------------------------
//  Screenshot (GET /screen.bmp)
//  The ST7789 panels here are write-only (readable=false, MISO unwired), so a
//  GRAM readback is impossible. Instead the current screen is re-rendered into
//  an offscreen 8-bit (RGB332) sprite by retargeting tft_ptr - the same
//  mechanism the JC3248W535 framebuffer uses - and streamed as a 24-bit BMP.
//  8-bit keeps the capture buffer at w*h bytes (57.6 KB on 240x240), which
//  fits the C3/CYD heap; colors are quantized but layout-faithful.
// ---------------------------------------------------------------------------
static void handleScreenshot() {
  lgfx::LovyanGFX* panel = tft_ptr;
  const int16_t w = (int16_t)panel->width();
  const int16_t h = (int16_t)panel->height();

  // Persistent capture sprite: allocated once and KEPT. Re-allocating per
  // request made the depth nondeterministic (16-bit when the heap happened to
  // have a 115 KB hole, 8-bit RGB332 otherwise), so consecutive previews
  // flipped between true color and a green-shifted tint - the "graphs change
  // colors" report. Prefer 16-bit, fall back to 8-bit, then stick with it.
  static lgfx::LGFX_Sprite spr;
  static int16_t capW = 0, capH = 0;
  if (capW != w || capH != h) {
    spr.deleteSprite();
    spr.setColorDepth(16);
    if (!spr.createSprite(w, h)) {
      spr.setColorDepth(8);
      if (!spr.createSprite(w, h)) {
        capW = capH = 0;
        server.send(503, "text/plain", "Not enough RAM for capture");
        return;
      }
    }
    capW = w;
    capH = h;
  }
  spr.fillSprite(dispSettings.bgColor);

  // Render one full frame into the sprite. The font cache and the gauge text
  // caches describe the panel, not the sprite - reset both around the swap.
  tft_ptr = &spr;
  resetFontCache();
  setCaptureRender(true);
  forceDisplayRedraw();
  updateDisplay();
  setCaptureRender(false);
  tft_ptr = panel;
  resetFontCache();
  forceDisplayRedraw();   // panel repaints (opaquely, no blanking) next loop tick

  const uint32_t rowBytes = (uint32_t)w * 3;   // w is a multiple of 4 on all boards
  const uint32_t imgBytes = rowBytes * (uint32_t)h;
  const uint32_t fileBytes = 54 + imgBytes;

  uint8_t* row = (uint8_t*)malloc(rowBytes);
  if (!row) {
    // Keep the persistent sprite - deleting it while capW/capH stay set would
    // make the next request render through a bufferless sprite.
    server.send(503, "text/plain", "Not enough RAM for row buffer");
    return;
  }

  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  hdr[2] = fileBytes & 0xFF; hdr[3] = (fileBytes >> 8) & 0xFF;
  hdr[4] = (fileBytes >> 16) & 0xFF; hdr[5] = (fileBytes >> 24) & 0xFF;
  hdr[10] = 54;                       // pixel data offset
  hdr[14] = 40;                       // BITMAPINFOHEADER
  hdr[18] = w & 0xFF; hdr[19] = (w >> 8) & 0xFF;
  hdr[22] = h & 0xFF; hdr[23] = (h >> 8) & 0xFF;
  hdr[26] = 1;                        // planes
  hdr[28] = 24;                       // bpp
  hdr[34] = imgBytes & 0xFF; hdr[35] = (imgBytes >> 8) & 0xFF;
  hdr[36] = (imgBytes >> 16) & 0xFF; hdr[37] = (imgBytes >> 24) & 0xFF;

  server.setContentLength(fileBytes);
  server.send(200, "image/bmp", "");
  WiFiClient client = server.client();
  client.write(hdr, sizeof(hdr));

  // BMP rows run bottom-up. readPixel() returns RGB565 regardless of the
  // sprite's storage depth, so one loop serves both capture modes.
  for (int16_t y = h - 1; y >= 0; y--) {
    for (int16_t x = 0; x < w; x++) {
      const uint16_t c = spr.readPixel(x, y);
      row[x * 3 + 0] = (uint8_t)((((c) & 0x1F) * 255) / 31);        // B (5 bit)
      row[x * 3 + 1] = (uint8_t)((((c >> 5) & 0x3F) * 255) / 63);   // G (6 bit)
      row[x * 3 + 2] = (uint8_t)((((c >> 11) & 0x1F) * 255) / 31);  // R (5 bit)
    }
    client.write(row, rowBytes);
  }

  free(row);
  // Sprite intentionally NOT deleted - see the allocation comment above.
}

// ---------------------------------------------------------------------------
//  OTA (multipart upload to /ota/upload)
// ---------------------------------------------------------------------------
static void handleOtaUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    otaError = "";
    otaInProgress = true;
    otaFirstChunk = true;
    setScreenState(SCREEN_OTA_UPDATE);
    updateDisplay();
    flushFrame();
    Serial.printf("OTA: start, file=%s\n", upload.filename.c_str());

    const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
      otaError = "No OTA partition found";
      otaInProgress = false;
      return;
    }
    if (!Update.begin(partition->size)) {
      otaError = "OTA begin failed: " + String(Update.errorString());
      otaInProgress = false;
      return;
    }
    if (server.hasHeader("X-MD5")) {
      String md5 = server.header("X-MD5");
      if (md5.length() == 32) Update.setMD5(md5.c_str());
    }

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (!otaInProgress) return;
    // Validate the ESP32 image magic byte on the first chunk.
    if (otaFirstChunk && upload.currentSize > 0) {
      otaFirstChunk = false;
      if (upload.buf[0] != 0xE9) {
        otaError = "Invalid firmware file";
        Update.abort();
        otaInProgress = false;
        return;
      }
    }
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      otaError = Update.errorString();
      Update.abort();
      otaInProgress = false;
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (!otaInProgress) return;
    if (Update.end(true)) {
      Serial.printf("OTA: success, %u bytes\n", upload.totalSize);
    } else {
      otaError = Update.errorString();
      Serial.printf("OTA: end failed: %s\n", otaError.c_str());
    }
    otaInProgress = false;

  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaInProgress = false;
    Serial.println("OTA: aborted");
  }
}

static void handleOtaFinish() {
  if (otaError.length() > 0) {
    server.send(400, "application/json",
                "{\"status\":\"error\",\"message\":\"" + otaError + "\"}");
    otaError = "";
    return;
  }
  server.send(200, "application/json",
    "{\"status\":\"ok\",\"message\":\"Update successful. Restarting...\"}");
  scheduleRestart(1500);
}

// ---------------------------------------------------------------------------
//  Captive portal
// ---------------------------------------------------------------------------
static void handleCaptiveDetect() {
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "");
}

static void handleNotFound() {
  if (isAPMode()) {
    handleCaptiveDetect();
  } else {
    server.send(404, "text/plain", "Not Found");
  }
}

// ---------------------------------------------------------------------------
//  Init & handle
// ---------------------------------------------------------------------------
void initWebServer() {
  server.on("/generate_204", HTTP_GET, handleCaptiveDetect);
  server.on("/gen_204", HTTP_GET, handleCaptiveDetect);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveDetect);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveDetect);
  server.on("/canonical.html", HTTP_GET, handleCaptiveDetect);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/portal.css", HTTP_GET, handlePortalCss);
  server.on("/portal-layout.css", HTTP_GET, handlePortalLayoutCss);
  server.on("/portal.js", HTTP_GET, handlePortalJs);
  server.on("/save/wifi", HTTP_POST, handleSaveWifiLegacy);
  server.on("/save/display", HTTP_POST, handleSaveDisplay);
  server.on("/save/gauges", HTTP_POST, handleSaveGauges);
  server.on("/save/colors", HTTP_POST, handleSaveColors);
  server.on("/save/clock", HTTP_POST, handleSaveClock);
  server.on("/save/network", HTTP_POST, handleSaveNetwork);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/screen.bmp", HTTP_GET, handleScreenshot);
  server.on("/ota/upload", HTTP_POST, handleOtaFinish, handleOtaUpload);

  const char* headers[] = { "X-MD5" };
  server.collectHeaders(headers, 1);

  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server started on :80");
}

void handleWebServer() {
  server.handleClient();
  if (restartAt != 0 && millis() >= restartAt) {
    Serial.println("Rebooting...");
    Serial.flush();
    ESP.restart();
  }
}
