#include "settings.h"
#include "config.h"
#include <Preferences.h>

// Global settings instances.
char wifiSSID[33] = "";
char wifiPass[65] = "";
uint8_t brightness = 200;
DisplaySettings dispSettings;
NetworkSettings netSettings;

static Preferences prefs;

// ---------------------------------------------------------------------------
//  Defaults
// ---------------------------------------------------------------------------
void defaultDisplaySettings(DisplaySettings& ds) {
  ds.rotation         = 0;
  ds.bgColor          = CLR_BG;
  ds.trackColor       = CLR_TRACK;
  ds.progressBarColor = CLR_GREEN;
  ds.animatedBar      = true;
  ds.smallLabels      = false;
  ds.invertColors     = false;
  ds.cydPanelClassic  = false;
  ds.pongClock        = false;
  ds.tempScaleMax     = GAUGE_TEMP_SCALE_DEFAULT;
  ds.powerScaleW      = GAUGE_POWER_SCALE_DEFAULT;
  ds.gaugeSmoothing   = 2;   // Normal
  ds.warnColor        = CLR_RED;
  ds.warnThresholdPct = 90;
  ds.clockTimeColor   = CLR_TEXT;
  ds.clockDateColor   = CLR_TEXT_DIM;
  ds.hideClockDate    = false;
  ds.progress = { CLR_GREEN, CLR_TEXT_DIM, CLR_TEXT };
  ds.gauge    = { CLR_BLUE,  CLR_TEXT_DIM, CLR_TEXT };
}

void defaultNetworkSettings(NetworkSettings& ns) {
  ns.useDHCP = true;
  ns.staticIP[0] = '\0';
  ns.gateway[0]  = '\0';
  ns.subnet[0]   = '\0';
  ns.dns[0]      = '\0';
  ns.showIPAtStartup = true;
  ns.timezoneIndex = 0;
  ns.timezoneStr[0] = '\0';
  ns.use24h = true;
  ns.dateFormat = 0;
  ns.mdnsEnabled = true;
  sanitizeHostname("pcmonitor", ns.hostname, sizeof(ns.hostname));
}

// ---------------------------------------------------------------------------
//  Persistence (NVS blobs - one key per struct, robust to field changes only
//  when the struct size matches; on size mismatch we fall back to defaults).
// ---------------------------------------------------------------------------
void loadSettings() {
  defaultDisplaySettings(dispSettings);
  defaultNetworkSettings(netSettings);

  prefs.begin(NVS_NAMESPACE, true);  // read-only

  if (prefs.getBytesLength("disp") == sizeof(DisplaySettings)) {
    prefs.getBytes("disp", &dispSettings, sizeof(DisplaySettings));
  }
  if (prefs.getBytesLength("net") == sizeof(NetworkSettings)) {
    prefs.getBytes("net", &netSettings, sizeof(NetworkSettings));
  }
  prefs.getString("ssid", wifiSSID, sizeof(wifiSSID));
  prefs.getString("pass", wifiPass, sizeof(wifiPass));
  brightness = prefs.getUChar("bright", 200);

  prefs.end();
}

void saveSettings() {
  prefs.begin(NVS_NAMESPACE, false);  // read-write
  prefs.putBytes("disp", &dispSettings, sizeof(DisplaySettings));
  prefs.putBytes("net", &netSettings, sizeof(NetworkSettings));
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPass);
  prefs.putUChar("bright", brightness);
  prefs.end();
}

// ---------------------------------------------------------------------------
//  RGB565 <-> HTML hex
// ---------------------------------------------------------------------------
uint16_t htmlToRgb565(const char* hex) {
  if (!hex) return 0;
  if (*hex == '#') hex++;
  long v = strtol(hex, nullptr, 16);
  uint8_t r = (v >> 16) & 0xFF;
  uint8_t g = (v >> 8) & 0xFF;
  uint8_t b = v & 0xFF;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void rgb565ToHtml(uint16_t color, char* buf) {
  uint8_t r = ((color >> 11) & 0x1F);
  uint8_t g = ((color >> 5) & 0x3F);
  uint8_t b = (color & 0x1F);
  // Scale 5/6-bit channels back to 8-bit.
  r = (r * 255 + 15) / 31;
  g = (g * 255 + 31) / 63;
  b = (b * 255 + 15) / 31;
  snprintf(buf, 8, "#%02X%02X%02X", r, g, b);
}

// ---------------------------------------------------------------------------
//  Hostname sanitizer
// ---------------------------------------------------------------------------
void sanitizeHostname(const char* in, char* out, size_t outSize) {
  if (outSize == 0) return;
  size_t j = 0;
  bool lastHyphen = false;
  for (size_t i = 0; in && in[i] && j < outSize - 1; i++) {
    char c = in[i];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
      lastHyphen = false;
    } else if ((c == '-' || c == ' ' || c == '_') && j > 0 && !lastHyphen) {
      out[j++] = '-';
      lastHyphen = true;
    }
  }
  // Strip trailing hyphen.
  while (j > 0 && out[j - 1] == '-') j--;
  out[j] = '\0';
  if (j == 0) {
    strncpy(out, "pcmonitor", outSize - 1);
    out[outSize - 1] = '\0';
  }
}
