// Slim web server for PCMonitorColor: a status page, a WiFi config form, an
// OTA upload endpoint (/ota/upload, same as BambuHelper), a JSON status API,
// and captive-portal redirects for the AP setup flow. The full gauge/metric
// configuration UI is added in the web-portal step.
#include "web_server.h"
#include "settings.h"
#include "wifi_manager.h"
#include "display_ui.h"
#include "pc_metrics.h"
#include "fonts.h"
#include "config.h"
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include <WiFi.h>

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
//  Gauge mapping config (per-slot metric / type / scale / color)
// ---------------------------------------------------------------------------
static const char* gaugeTypeName(uint8_t t) {
  switch (t) {
    case GAUGE_TYPE_AUTO:    return "Auto";
    case GAUGE_TYPE_TEMP:    return "Temp";
    case GAUGE_TYPE_PERCENT: return "Percent";
    case GAUGE_TYPE_POWER:   return "Power";
    case GAUGE_TYPE_FAN:     return "Fan";
    default:                 return "?";
  }
}

static const char* displayStyleName(uint8_t s) {
  switch (s) {
    case STYLE_RINGS:       return "Rings";
    case STYLE_BIG_NUMBERS: return "Big numbers";
    case STYLE_TILES:       return "Tiles + sparklines";
    case STYLE_HERO:        return "Hero + list";
    default:                return "?";
  }
}

static void appendGaugesCard(String& html) {
  html += "<div class='card'><b>Display</b>";

  html += "<form method='POST' action='/save/gauges'>";
  html += "<label>Style</label><select name='style'>";
  for (uint8_t s = 0; s < STYLE_COUNT; s++) {
    html += "<option value='" + String(s) + "'";
    if (displayStyle == s) html += " selected";
    html += ">" + String(displayStyleName(s)) + "</option>";
  }
  html += "</select>";
  html += "<label>Chart refresh (seconds, 1-60)</label>";
  html += "<input style='width:90px' type='number' min='1' max='60' name='sparks' value='" +
          String(sparkRedrawSec) + "'>";

  // Live list of metrics the companion is sending, so the user knows which ids
  // are available to bind.
  html += "<div style='font-size:12px;color:#9aa4ad;margin:6px 0'>Available metrics: ";
  if (pcData.count == 0) {
    html += "(none yet - start the PC companion)";
  } else {
    for (uint8_t i = 0; i < pcData.count; i++) {
      const PcMetric& m = pcData.metrics[i];
      html += String(m.id) + ":" + String(m.name) + " (" + String(m.unit) + ")";
      if (i + 1 < pcData.count) html += ", ";
    }
  }
  html += "</div>";

  html += "<table style='width:100%;border-collapse:collapse;font-size:13px'>";
  html += "<tr style='color:#9aa4ad;text-align:left'>"
          "<th>Slot</th><th>Metric</th><th>Type</th><th>Scale</th><th>Color</th><th></th></tr>";

  char colBuf[8];
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    const GaugeSlot& s = gaugeMap.slots[i];
    rgb565ToHtml(s.arcColor, colBuf);

    html += "<tr><td>" + String(i + 1) + "</td>";
    // Metric picker: live metrics by name; 0 = slot off. If the bound id is
    // not in the current packet (companion off / rebind), keep it selectable.
    html += "<td><select name='m" + String(i) + "'>";
    html += "<option value='0'";
    if (s.metricId == 0) html += " selected";
    html += ">(off)</option>";
    bool bound = (s.metricId == 0);
    for (uint8_t k = 0; k < pcData.count; k++) {
      const PcMetric& m = pcData.metrics[k];
      html += "<option value='" + String(m.id) + "'";
      if (s.metricId == m.id) { html += " selected"; bound = true; }
      html += ">" + String(m.id) + ": " + String(m.name) + " (" + String(m.unit) + ")</option>";
    }
    if (!bound) {
      html += "<option value='" + String(s.metricId) + "' selected>id " +
              String(s.metricId) + " (offline)</option>";
    }
    html += "</select></td>";
    // Type select
    html += "<td><select name='t" + String(i) + "'>";
    for (uint8_t t = 0; t < GAUGE_TYPE_COUNT; t++) {
      html += "<option value='" + String(t) + "'";
      if (s.type == t) html += " selected";
      html += ">" + String(gaugeTypeName(t)) + "</option>";
    }
    html += "</select></td>";
    // Scale (0 = type default)
    html += "<td><input style='width:70px' type='number' min='0' max='65535' name='s" + String(i) +
            "' value='" + String(s.scaleMax) + "' placeholder='auto'></td>";
    // Color
    html += "<td><input type='color' name='c" + String(i) + "' value='" + String(colBuf) + "'></td>";
    // Reorder (swaps the whole row's values client-side; save to apply)
    html += "<td style='white-space:nowrap'>"
            "<button type='button' class='mv' onclick='mv(" + String(i) + ",-1)'>&#9650;</button>"
            "<button type='button' class='mv' onclick='mv(" + String(i) + ",1)'>&#9660;</button></td>";
    html += "</tr>";
  }
  html += "</table>";
  html += "<small>Metric id binds the slot to a PC sensor by id (0 = off). "
          "Scale 0 = type default. Auto picks the gauge style from the unit. "
          "Slot order is the reading order in every style; slot 1 is the hero.</small><br>";
  html += "<button type='submit'>Save display</button></form></div>";
}

// ---------------------------------------------------------------------------
//  Display colors (bg/track/warn/clock pickers + one-click presets)
// ---------------------------------------------------------------------------
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

static void appendColorsCard(String& html) {
  char buf[8];
  html += "<div class='card'><b>Colors</b>";
  html += "<form method='POST' action='/save/colors'>";
  html += "<label>Preset</label><select name='preset'>";
  html += "<option value='' selected>(custom - use the pickers below)</option>";
  html += "<option value='default'>Factory default</option>";
  html += "<option value='modern'>Modern (colorblind-safe)</option>";
  html += "</select>";
  html += "<table style='font-size:13px'><tr>";
  rgb565ToHtml(dispSettings.bgColor, buf);
  html += "<td>Background<br><input type='color' name='cbg' value='" + String(buf) + "'></td>";
  rgb565ToHtml(dispSettings.trackColor, buf);
  html += "<td>Track<br><input type='color' name='ctrack' value='" + String(buf) + "'></td>";
  rgb565ToHtml(dispSettings.warnColor, buf);
  html += "<td>Warn<br><input type='color' name='cwarn' value='" + String(buf) + "'></td>";
  rgb565ToHtml(dispSettings.clockTimeColor, buf);
  html += "<td>Clock<br><input type='color' name='cct' value='" + String(buf) + "'></td>";
  rgb565ToHtml(dispSettings.clockDateColor, buf);
  html += "<td>Date<br><input type='color' name='ccd' value='" + String(buf) + "'></td>";
  html += "</tr></table>";
  html += "<small>A preset overwrites these pickers AND the six slot colors in "
          "one save; leave it on custom to apply the pickers only.</small><br>";
  html += "<button type='submit'>Save colors</button></form></div>";
}

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
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Saved");
}

// ---------------------------------------------------------------------------
//  Status page
// ---------------------------------------------------------------------------
static void appendPreviewCard(String& html) {
  html += "<div class='card'><b>Device preview</b><br>";
  html += "<img id='scr' src='/screen.bmp' width='240' "
          "style='margin-top:8px;border:1px solid #2b3036;border-radius:8px;"
          "image-rendering:pixelated'>";
  html += "<br><label style='display:inline'><input type='checkbox' id='auto' checked "
          "style='width:auto'> Auto-refresh</label></div>";
}

static void handleRoot() {
  String ip = WiFi.localIP().toString();
  String html;
  html.reserve(8192);
  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" PRODUCT_NAME "</title><style>";
  html += "body{font-family:system-ui,sans-serif;background:#15181c;color:#e6e6e6;margin:0;padding:16px}";
  html += "h1{font-size:20px;color:#3FB57A}.card{background:#1f2329;border:1px solid #2b3036;border-radius:10px;padding:14px;margin:12px 0}";
  html += "label{display:block;margin:8px 0 4px;font-size:13px;color:#9aa4ad}";
  html += "input{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #2b3036;background:#15181c;color:#e6e6e6}";
  html += "button{margin-top:10px;padding:9px 14px;border:0;border-radius:6px;background:#3FB57A;color:#08130c;font-weight:600;cursor:pointer}";
  html += "select,input[type=number]{padding:6px;border-radius:6px;border:1px solid #2b3036;background:#15181c;color:#e6e6e6}";
  html += "td,th{padding:4px 6px}small{color:#778}";
  html += "button.mv{margin:0 1px;padding:4px 7px;background:#2b3036;color:#9aa4ad}";
  html += "</style></head><body>";
  html += "<h1>" PRODUCT_NAME " " FW_VERSION "</h1>";

  html += "<div class='card'><b>Status</b><br>";
  html += "Board: " BOARD_VARIANT "<br>";
  html += "IP: " + ip + "<br>";
  html += "PC: " + String(pcData.online ? "online" : "offline");
  html += " (" + String(pcData.count) + " metrics)";
  if (pcData.timestamp[0]) html += " @ " + String(pcData.timestamp);
  html += "</div>";

  html += "<div class='card'><b>WiFi</b><form method='POST' action='/save/wifi'>";
  html += "<label>SSID</label><input name='ssid' value='" + String(wifiSSID) + "'>";
  html += "<label>Password</label><input name='pass' type='password' placeholder='(unchanged)'>";
  html += "<button type='submit'>Save &amp; reboot</button></form></div>";

  appendPreviewCard(html);
  appendGaugesCard(html);
  appendColorsCard(html);

  html += "<div class='card'><b>Firmware update</b>";
  html += "<form method='POST' action='/ota/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware' accept='.bin'>";
  html += "<button type='submit'>Upload &amp; flash</button></form>";
  html += "<small>POST a .bin to /ota/upload</small></div>";

  // Preview auto-refresh + slot reorder (swaps row values; Save applies).
  // Preview refresh is double-buffered: load into an offscreen Image and swap
  // src only when complete, otherwise the browser blanks the img for the 1-2 s
  // the ESP32 needs to stream the BMP and the preview "jumps" every cycle.
  html += "<script>"
          "var busy=false;"
          "setInterval(function(){var a=document.getElementById('auto');"
          "if(a&&a.checked&&!busy){busy=true;var n=new Image();"
          "n.onload=function(){document.getElementById('scr').src=n.src;busy=false;};"
          "n.onerror=function(){busy=false;};"
          "n.src='/screen.bmp?t='+Date.now();}},5000);"
          "function mv(i,d){var j=i+d;if(j<0||j>=" + String(NUM_GAUGE_SLOTS) + ")return;"
          "['m','t','s','c'].forEach(function(p){"
          "var A=document.getElementsByName(p+i)[0],B=document.getElementsByName(p+j)[0];"
          "var v=A.value;A.value=B.value;B.value=v;});}"
          "</script>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

static void handleSaveWifi() {
  if (server.hasArg("ssid")) {
    strlcpy(wifiSSID, server.arg("ssid").c_str(), sizeof(wifiSSID));
  }
  // Only overwrite the password when a new one was actually entered.
  if (server.hasArg("pass") && server.arg("pass").length() > 0) {
    strlcpy(wifiPass, server.arg("pass").c_str(), sizeof(wifiPass));
  }
  saveSettings();
  server.send(200, "text/html",
    "<!DOCTYPE html><meta http-equiv='refresh' content='4;url=/'>"
    "<body style='font-family:sans-serif;background:#15181c;color:#e6e6e6'>"
    "Saved. Rebooting...</body>");
  scheduleRestart(1200);
}

static void handleSaveGauges() {
  if (server.hasArg("style")) {
    long v = server.arg("style").toInt();
    if (v >= 0 && v < STYLE_COUNT) displayStyle = (uint8_t)v;
  }
  if (server.hasArg("sparks")) {
    long v = server.arg("sparks").toInt();
    if (v >= 1 && v <= 60) sparkRedrawSec = (uint8_t)v;
  }
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    GaugeSlot& s = gaugeMap.slots[i];
    char key[4];

    snprintf(key, sizeof(key), "m%u", i);
    if (server.hasArg(key)) {
      long v = server.arg(key).toInt();
      if (v < 0) v = 0;
      if (v > MAX_METRICS) v = MAX_METRICS;
      s.metricId = (uint8_t)v;
    }
    snprintf(key, sizeof(key), "t%u", i);
    if (server.hasArg(key)) {
      long t = server.arg(key).toInt();
      if (t >= 0 && t < GAUGE_TYPE_COUNT) s.type = (uint8_t)t;
    }
    snprintf(key, sizeof(key), "s%u", i);
    if (server.hasArg(key)) {
      long v = server.arg(key).toInt();
      if (v < 0) v = 0;
      if (v > 65535) v = 65535;
      s.scaleMax = (uint16_t)v;
    }
    snprintf(key, sizeof(key), "c%u", i);
    if (server.hasArg(key)) s.arcColor = htmlToRgb565(server.arg(key).c_str());
  }
  saveSettings();
  forceDisplayRedraw();   // apply the new mapping live, no reboot
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Saved");
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
  server.on("/save/wifi", HTTP_POST, handleSaveWifi);
  server.on("/save/gauges", HTTP_POST, handleSaveGauges);
  server.on("/save/colors", HTTP_POST, handleSaveColors);
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
