// Slim web server for PCMonitorColor: a status page, a WiFi config form, an
// OTA upload endpoint (/ota/upload, same as BambuHelper), a JSON status API,
// and captive-portal redirects for the AP setup flow. The full gauge/metric
// configuration UI is added in the web-portal step.
#include "web_server.h"
#include "settings.h"
#include "wifi_manager.h"
#include "display_ui.h"
#include "pc_metrics.h"
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
//  Status page
// ---------------------------------------------------------------------------
static void handleRoot() {
  String ip = WiFi.localIP().toString();
  String html;
  html.reserve(2048);
  html += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>" PRODUCT_NAME "</title><style>";
  html += "body{font-family:system-ui,sans-serif;background:#15181c;color:#e6e6e6;margin:0;padding:16px}";
  html += "h1{font-size:20px;color:#3FB57A}.card{background:#1f2329;border:1px solid #2b3036;border-radius:10px;padding:14px;margin:12px 0}";
  html += "label{display:block;margin:8px 0 4px;font-size:13px;color:#9aa4ad}";
  html += "input{width:100%;box-sizing:border-box;padding:8px;border-radius:6px;border:1px solid #2b3036;background:#15181c;color:#e6e6e6}";
  html += "button{margin-top:10px;padding:9px 14px;border:0;border-radius:6px;background:#3FB57A;color:#08130c;font-weight:600;cursor:pointer}";
  html += "small{color:#778}</style></head><body>";
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

  html += "<div class='card'><b>Firmware update</b>";
  html += "<form method='POST' action='/ota/upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware' accept='.bin'>";
  html += "<button type='submit'>Upload &amp; flash</button></form>";
  html += "<small>POST a .bin to /ota/upload</small></div>";

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

static void handleApiStatus() {
  JsonDocument doc;
  doc["product"] = PRODUCT_NAME;
  doc["version"] = FW_VERSION;
  doc["board"]   = BOARD_VARIANT;
  doc["ip"]      = WiFi.localIP().toString();
  doc["pc_online"] = pcData.online;
  doc["pc_status"] = pcData.status;
  doc["metric_count"] = pcData.count;
  doc["timestamp"] = pcData.timestamp;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
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
  server.on("/api/status", HTTP_GET, handleApiStatus);
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
