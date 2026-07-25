#include "settings.h"
#include "config.h"
#include <Preferences.h>

// Global settings instances.
char wifiSSID[33] = "";
char wifiPass[65] = "";
uint8_t brightness = 200;
DisplaySettings dispSettings;
NetworkSettings netSettings;
GaugeMapping gaugeMap;
GaugeLabels gaugeLabels;
BacklightSettings backlightSettings;
TouchSettings touchSettings;
ThemeSettings themeSettings;
uint8_t displayStyle = STYLE_DEFAULT;
uint8_t clockFace = CLOCK_FACE_STANDARD;
uint8_t sparkRedrawSec = SPARK_REDRAW_MS / 1000;

static Preferences prefs;
static const uint8_t BACKLIGHT_SETTINGS_VERSION = 1;
static const uint8_t TOUCH_SETTINGS_VERSION = 1;
static const uint8_t THEME_SETTINGS_VERSION = 1;

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

void defaultGaugeMapping(GaugeMapping& gm) {
  // Out-of-box mapping: bind metric ids 1..N to the slots and let each gauge
  // auto-classify by the metric's unit (same behavior the renderer had before
  // the mapping was configurable). The user overrides any slot from the portal.
  static const uint16_t palette[NUM_GAUGE_SLOTS] = {
    CLR_ORANGE, CLR_BLUE, CLR_GREEN, CLR_CYAN, CLR_GOLD, CLR_RED,
    CLR_VIOLET, CLR_ROSE
  };
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    // Board-dependent: eight on the 320x480 panel, six on the 240x240 squares
    // (see DEFAULT_BOUND_SLOTS). Slots past that start empty, so a device never
    // boots into more readings than its panel carries well, and the user adds or
    // removes tiles by binding/clearing slots on the portal's Metrics page.
    gm.slots[i].metricId = (i < DEFAULT_BOUND_SLOTS) ? (uint8_t)(i + 1) : 0;
    gm.slots[i].type     = GAUGE_TYPE_AUTO;
    gm.slots[i].scaleMax = 0;            // 0 = use the gauge type's default scale
    gm.slots[i].arcColor = palette[i];
  }
}

void defaultGaugeLabels(GaugeLabels& labels) {
  memset(&labels, 0, sizeof(labels));
}

void defaultBacklightSettings(BacklightSettings& bs) {
  bs.version = BACKLIGHT_SETTINGS_VERSION;
  bs.nightEnabled = 0;
  bs.nightBrightness = 32;
  bs.nightOfflineOff = 0;
  bs.nightStartMinute = 22 * 60;
  bs.nightEndMinute = 7 * 60;
  bs.offlineSleepMinutes = 0;
}

void defaultTouchSettings(TouchSettings& ts) {
  ts.version = TOUCH_SETTINGS_VERSION;
  ts.enabled = 0;
  ts.pin = 3;
  ts.activeHigh = 1;
  ts.shortAction = TOUCH_ACTION_NEXT_STYLE;
  ts.longAction = TOUCH_ACTION_TOGGLE_POWER;
  ts.styleMask = STYLE_ACTIVE_MASK;
  ts.rememberStyle = 0;
}

void defaultThemeSettings(ThemeSettings& ts) {
  ts.version = THEME_SETTINGS_VERSION;
  ts.labelMode = THEME_LABEL_CLASSIC;
  ts.tileTintPct = 0;
  ts.reserved = 0;
  ts.valueColor = CLR_TEXT;
  ts.labelColor = CLR_TEXT_DIM;
  ts.secondaryColor = CLR_TEXT_DIM;
  ts.tileColor = CLR_CARD;
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
  sanitizeHostname("statnode", ns.hostname, sizeof(ns.hostname));
}

// ---------------------------------------------------------------------------
//  Persistence (NVS blobs - one key per struct, robust to field changes only
//  when the struct size matches; on size mismatch we fall back to defaults).
// ---------------------------------------------------------------------------
void loadSettings() {
  defaultDisplaySettings(dispSettings);
  defaultNetworkSettings(netSettings);
  defaultGaugeMapping(gaugeMap);
  defaultGaugeLabels(gaugeLabels);
  defaultBacklightSettings(backlightSettings);
  defaultTouchSettings(touchSettings);
  defaultThemeSettings(themeSettings);

  prefs.begin(NVS_NAMESPACE, true);  // read-only

  if (prefs.getBytesLength("disp") == sizeof(DisplaySettings)) {
    prefs.getBytes("disp", &dispSettings, sizeof(DisplaySettings));
  }
  if (prefs.getBytesLength("net") == sizeof(NetworkSettings)) {
    prefs.getBytes("net", &netSettings, sizeof(NetworkSettings));
  }
  // Gauge mapping + labels are sized by NUM_GAUGE_SLOTS, so raising the slot
  // count changes the blob length. The exact-size checks below would then miss
  // and silently fall back to defaults, resetting every slot the user bound. So
  // also accept the older, shorter layout and copy what it holds into the
  // leading slots; the new trailing slots stay at their defaults (unbound is
  // the honest state - nothing was ever configured for them).
  {
    struct LegacyGaugeMappingV1 { GaugeSlot slots[LEGACY_GAUGE_SLOTS_V1]; };
    const size_t stored = prefs.getBytesLength("gauges");
    if (stored == sizeof(GaugeMapping)) {
      prefs.getBytes("gauges", &gaugeMap, sizeof(GaugeMapping));
    } else if (stored == sizeof(LegacyGaugeMappingV1)) {
      LegacyGaugeMappingV1 old;
      prefs.getBytes("gauges", &old, sizeof(old));
      for (uint8_t i = 0; i < LEGACY_GAUGE_SLOTS_V1; i++) gaugeMap.slots[i] = old.slots[i];
      Serial.printf("Settings: migrated %u-slot gauge mapping to %u slots\n",
                    (unsigned)LEGACY_GAUGE_SLOTS_V1, (unsigned)NUM_GAUGE_SLOTS);
    }
  }
  {
    struct LegacyGaugeLabelsV1 { char labels[LEGACY_GAUGE_SLOTS_V1][GAUGE_LABEL_LENGTH]; };
    const size_t stored = prefs.getBytesLength("labels");
    if (stored == sizeof(GaugeLabels)) {
      prefs.getBytes("labels", &gaugeLabels, sizeof(GaugeLabels));
    } else if (stored == sizeof(LegacyGaugeLabelsV1)) {
      LegacyGaugeLabelsV1 old;
      prefs.getBytes("labels", &old, sizeof(old));
      for (uint8_t i = 0; i < LEGACY_GAUGE_SLOTS_V1; i++)
        strlcpy(gaugeLabels.labels[i], old.labels[i], GAUGE_LABEL_LENGTH);
    }
  }
  if (prefs.getBytesLength("backlight") == sizeof(BacklightSettings)) {
    BacklightSettings stored;
    prefs.getBytes("backlight", &stored, sizeof(stored));
    if (stored.version == BACKLIGHT_SETTINGS_VERSION) backlightSettings = stored;
  }
  if (prefs.getBytesLength("touch") == sizeof(TouchSettings)) {
    TouchSettings stored;
    prefs.getBytes("touch", &stored, sizeof(stored));
    if (stored.version == TOUCH_SETTINGS_VERSION) touchSettings = stored;
  }
  if (prefs.getBytesLength("theme") == sizeof(ThemeSettings)) {
    ThemeSettings stored;
    prefs.getBytes("theme", &stored, sizeof(stored));
    if (stored.version == THEME_SETTINGS_VERSION) themeSettings = stored;
  }
  prefs.getString("ssid", wifiSSID, sizeof(wifiSSID));
  prefs.getString("pass", wifiPass, sizeof(wifiPass));
  brightness = prefs.getUChar("bright", 200);
  const uint8_t storedDisplayStyle = prefs.getUChar("style", STYLE_DEFAULT);
  displayStyle = normalizeDisplayStyle(storedDisplayStyle);
  const bool migrateDisplayStyle = displayStyle != storedDisplayStyle;
  clockFace = prefs.getUChar(
    "clockface", dispSettings.pongClock ? CLOCK_FACE_BREAKOUT : CLOCK_FACE_STANDARD);
  if (clockFace >= CLOCK_FACE_COUNT) clockFace = CLOCK_FACE_STANDARD;
  dispSettings.pongClock = clockFace == CLOCK_FACE_BREAKOUT;
  sparkRedrawSec = prefs.getUChar("sparks", SPARK_REDRAW_MS / 1000);
  if (sparkRedrawSec < 1) sparkRedrawSec = 1;
  if (sparkRedrawSec > 60) sparkRedrawSec = 60;
  backlightSettings.nightEnabled = backlightSettings.nightEnabled ? 1 : 0;
  backlightSettings.nightOfflineOff = backlightSettings.nightOfflineOff ? 1 : 0;
  if (backlightSettings.nightStartMinute >= 24 * 60)
    backlightSettings.nightStartMinute = 22 * 60;
  if (backlightSettings.nightEndMinute >= 24 * 60)
    backlightSettings.nightEndMinute = 7 * 60;
  if (backlightSettings.offlineSleepMinutes > 24 * 60)
    backlightSettings.offlineSleepMinutes = 24 * 60;
  touchSettings.enabled = touchSettings.enabled ? 1 : 0;
  touchSettings.activeHigh = touchSettings.activeHigh ? 1 : 0;
  touchSettings.rememberStyle = touchSettings.rememberStyle ? 1 : 0;
  if (touchSettings.shortAction >= TOUCH_ACTION_COUNT)
    touchSettings.shortAction = TOUCH_ACTION_NEXT_STYLE;
  if (touchSettings.longAction >= TOUCH_ACTION_COUNT)
    touchSettings.longAction = TOUCH_ACTION_TOGGLE_POWER;
  const uint8_t storedTouchStyleMask = touchSettings.styleMask;
  touchSettings.styleMask &= STYLE_ACTIVE_MASK;
  if (touchSettings.styleMask == 0)
    touchSettings.styleMask = 1u << STYLE_DEFAULT;
  const bool migrateTouchStyleMask =
    touchSettings.styleMask != storedTouchStyleMask;
  if (themeSettings.labelMode >= THEME_LABEL_MODE_COUNT)
    themeSettings.labelMode = THEME_LABEL_CLASSIC;
  if (themeSettings.tileTintPct > 30) themeSettings.tileTintPct = 30;
  for (uint8_t i = 0; i < NUM_GAUGE_SLOTS; i++) {
    gaugeLabels.labels[i][GAUGE_LABEL_LENGTH - 1] = '\0';
    char cleaned[GAUGE_LABEL_LENGTH];
    sanitizeGaugeLabel(gaugeLabels.labels[i], cleaned, sizeof(cleaned));
    strlcpy(gaugeLabels.labels[i], cleaned, sizeof(gaugeLabels.labels[i]));
  }

  prefs.end();

  if (migrateDisplayStyle || migrateTouchStyleMask) {
    prefs.begin(NVS_NAMESPACE, false);
    if (migrateDisplayStyle) prefs.putUChar("style", displayStyle);
    if (migrateTouchStyleMask)
      prefs.putBytes("touch", &touchSettings, sizeof(TouchSettings));
    prefs.end();
  }
}

void saveSettings() {
  // Keep the old field coherent for downgrade compatibility. Mario maps to
  // Standard on firmware versions that predate the dedicated scalar.
  dispSettings.pongClock = clockFace == CLOCK_FACE_BREAKOUT;
  displayStyle = normalizeDisplayStyle(displayStyle);
  touchSettings.styleMask &= STYLE_ACTIVE_MASK;
  if (touchSettings.styleMask == 0)
    touchSettings.styleMask = 1u << STYLE_DEFAULT;
  prefs.begin(NVS_NAMESPACE, false);  // read-write
  prefs.putBytes("disp", &dispSettings, sizeof(DisplaySettings));
  prefs.putBytes("net", &netSettings, sizeof(NetworkSettings));
  prefs.putBytes("gauges", &gaugeMap, sizeof(GaugeMapping));
  prefs.putBytes("labels", &gaugeLabels, sizeof(GaugeLabels));
  prefs.putBytes("backlight", &backlightSettings, sizeof(BacklightSettings));
  prefs.putBytes("touch", &touchSettings, sizeof(TouchSettings));
  prefs.putBytes("theme", &themeSettings, sizeof(ThemeSettings));
  prefs.putString("ssid", wifiSSID);
  prefs.putString("pass", wifiPass);
  prefs.putUChar("bright", brightness);
  prefs.putUChar("style", displayStyle);
  prefs.putUChar("clockface", clockFace);
  prefs.putUChar("sparks", sparkRedrawSec);
  prefs.end();
}

bool factoryResetSettings() {
  prefs.begin(NVS_NAMESPACE, false);
  const bool cleared = prefs.clear();
  prefs.end();
  return cleared;
}

void saveDisplayStyle() {
  displayStyle = normalizeDisplayStyle(displayStyle);
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar("style", displayStyle);
  prefs.end();
}

void sanitizeGaugeLabel(const char* in, char* out, size_t outSize) {
  if (!out || outSize == 0) return;
  out[0] = '\0';
  if (!in) return;

  while (*in == ' ') in++;
  size_t written = 0;
  for (; *in && written < outSize - 1; in++) {
    const uint8_t c = (uint8_t)*in;
    if (c >= 32 && c <= 126) out[written++] = (char)c;
  }
  while (written > 0 && out[written - 1] == ' ') written--;
  out[written] = '\0';
}

const char* gaugeDisplayLabel(uint8_t slotIndex, const char* fallback) {
  if (slotIndex < NUM_GAUGE_SLOTS && gaugeLabels.labels[slotIndex][0])
    return gaugeLabels.labels[slotIndex];
  return fallback ? fallback : "";
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
    strncpy(out, "statnode", outSize - 1);
    out[outSize - 1] = '\0';
  }
}
