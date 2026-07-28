#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include "config.h"   // NUM_GAUGE_SLOTS

// Per-gauge color config (RGB565). Mirrors the BambuHelper GaugeColors so the
// reused gauge primitives in display_gauges.cpp compile unchanged.
struct GaugeColors {
  uint16_t arc;       // arc fill color
  uint16_t label;     // label text color
  uint16_t value;     // value text color
};

// Gauge rendering style for a configurable slot.
enum GaugeType : uint8_t {
  GAUGE_TYPE_AUTO    = 0,  // classify by the metric's unit at render time
  GAUGE_TYPE_TEMP    = 1,  // raw value on a 0..scale arc, warn coloring (C)
  GAUGE_TYPE_PERCENT = 2,  // 0..100 percent gauge (%)
  GAUGE_TYPE_POWER   = 3,  // watts with W/kW readout, arc 0..scale (W)
  GAUGE_TYPE_FAN     = 4,  // raw value on a 0..scale arc (RPM / generic)
  GAUGE_TYPE_COUNT
};

// One configurable gauge slot. Binds a PC metric (STRICTLY by id, matching the
// V2.1 protocol) to a screen position with a chosen gauge style, arc full-scale,
// and accent color. The slot's label comes from the live metric name, so it
// follows whatever the companion reports. Persisted as the NVS "gauges" blob.
struct GaugeSlot {
  uint8_t  metricId;   // PC metric id to show; 0 = slot empty
  uint8_t  type;       // GaugeType
  uint16_t scaleMax;   // arc full-scale; 0 = use the type default
  uint16_t arcColor;   // arc + accent color (RGB565)
};

struct GaugeMapping {
  GaugeSlot slots[NUM_GAUGE_SLOTS];
};

// Optional display names are stored separately from GaugeMapping so adding
// them does not invalidate an existing metric layout after OTA. Empty means
// use the live metric name reported by the companion.
static const size_t GAUGE_LABEL_LENGTH = 17;  // 16 printable characters + NUL
struct GaugeLabels {
  char labels[NUM_GAUGE_SLOTS][GAUGE_LABEL_LENGTH];
};

// Backlight automation is also a separate versioned NVS blob. `brightness`
// remains the normal daytime level for backwards compatibility.
struct BacklightSettings {
  uint8_t  version;
  uint8_t  nightEnabled;
  uint8_t  nightBrightness;
  uint8_t  nightOfflineOff;      // display fully off while PC offline inside
                                 // the night interval (was the reserved byte,
                                 // so old blobs read back as disabled)
  uint16_t nightStartMinute;     // minute of day, 0..1439
  uint16_t nightEndMinute;       // minute of day, 0..1439
  uint16_t offlineSleepMinutes;  // 0 disables PC-offline sleep
};

// Monitor screen face. Selects how the bound metrics are rendered; all styles
// share the same GaugeMapping slots (slot order = reading order, slot 1 = hero
// in STYLE_HERO). Persisted as its own NVS scalar ("style"), NOT inside the
// DisplaySettings blob, so adding it never resizes "disp" (a size mismatch
// there would reset every display setting on OTA).
enum DisplayStyle : uint8_t {
  STYLE_RETIRED     = 0,  // legacy persisted value, migrated to Tiles
  STYLE_BIG_NUMBERS = 1,  // large values + hairline meter, no chrome
  STYLE_TILES       = 2,  // cards with value + sparkline history
  STYLE_HERO        = 3,  // slot 1 large with sparkline + compact rows
  STYLE_STRIPS      = 4,  // full-width sparkline lane per metric
  STYLE_DUO         = 5,  // slots 1+2 as hero bands + compact grid
  STYLE_PULSE       = 6,  // accent-washed blocks, tint follows load
  STYLE_GLASS_TILES = 7,  // Tiles geometry on Aero glass panes
  STYLE_GLASS_DUO   = 8,  // Duo geometry on edge-lit glass capsules
  STYLE_COUNT
};

constexpr uint8_t STYLE_DEFAULT = STYLE_TILES;
// Widened to 16 bits when the glass faces pushed the highest style past bit 7.
// Everything that stores or transports a style mask must match this width.
constexpr uint16_t STYLE_ACTIVE_MASK =
  (1u << STYLE_BIG_NUMBERS) | (1u << STYLE_TILES) | (1u << STYLE_HERO) |
  (1u << STYLE_STRIPS) | (1u << STYLE_DUO) | (1u << STYLE_PULSE) |
  (1u << STYLE_GLASS_TILES) | (1u << STYLE_GLASS_DUO);

// True for the faces that paint a gradient backdrop instead of a flat fill.
// Anything that would otherwise clear a region to dispSettings.bgColor has to
// branch on this - a flat clear punches a visible hole through the gradient.
inline bool styleUsesGlass(uint8_t style) {
  return style == STYLE_GLASS_TILES || style == STYLE_GLASS_DUO;
}

inline uint8_t normalizeDisplayStyle(uint8_t style) {
  return (style >= STYLE_BIG_NUMBERS && style < STYLE_COUNT)
    ? style : STYLE_DEFAULT;
}

// Idle clock face. Stored as its own NVS scalar ("clockface") so extending
// the list cannot resize the legacy DisplaySettings blob and reset unrelated
// display configuration after an OTA update.
enum ClockFace : uint8_t {
  CLOCK_FACE_STANDARD = 0,
  CLOCK_FACE_BREAKOUT = 1,
  CLOCK_FACE_RUNNER = 2,
  CLOCK_FACE_COUNT
};

enum TouchAction : uint8_t {
  TOUCH_ACTION_NONE         = 0,
  TOUCH_ACTION_NEXT_STYLE   = 1,
  TOUCH_ACTION_PREV_STYLE   = 2,
  TOUCH_ACTION_TOGGLE_CLOCK = 3,
  TOUCH_ACTION_TOGGLE_POWER = 4,
  TOUCH_ACTION_COUNT
};

// TTP223 settings use their own versioned blob so adding touch support cannot
// invalidate existing display or network configuration after OTA.
struct TouchSettings {
  uint8_t  version;
  uint8_t  enabled;
  uint8_t  pin;
  uint8_t  activeHigh;
  uint8_t  shortAction;
  uint8_t  longAction;
  uint8_t  rememberStyle;
  uint16_t styleMask;   // v2: widened, the glass faces need bits 7 and 8
};

// v1 layout, kept so an existing device keeps its pad configuration across the
// OTA that widened styleMask. Read-only: see loadSettings' migration.
struct TouchSettingsV1 {
  uint8_t version;
  uint8_t enabled;
  uint8_t pin;
  uint8_t activeHigh;
  uint8_t shortAction;
  uint8_t longAction;
  uint8_t styleMask;
  uint8_t rememberStyle;
};

// Status LED settings get their own versioned blob for the same reason the
// touch settings do: adding them must not invalidate display or network
// configuration after an OTA update. A single-color LED on a PWM pin, so
// there is no color field - see src/led.cpp.
struct LedSettings {
  uint8_t version;
  uint8_t enabled;
  uint8_t pin;
  uint8_t brightness;       // 0..255, the normal daytime level
  uint8_t nightEnabled;     // use nightBrightness inside the night interval
  uint8_t nightBrightness;  // 0..255, 0 switches the LED fully off at night
  uint8_t followDisplay;    // dark whenever the panel itself is dark
  uint8_t offlineOff;       // dark while the PC companion is offline
};

enum ThemeLabelMode : uint8_t {
  THEME_LABEL_CLASSIC = 0,  // preserve each layout's original label treatment
  THEME_LABEL_CUSTOM  = 1,  // use ThemeSettings::labelColor
  THEME_LABEL_ACCENT  = 2,  // match the metric slot accent
  THEME_LABEL_AUTO    = 3,  // choose black or white for local contrast
  THEME_LABEL_MODE_COUNT
};

// Monitor-face typography and tile colors use a separate versioned blob so
// adding theme controls cannot invalidate the older DisplaySettings blob.
struct ThemeSettings {
  uint8_t  version;
  uint8_t  labelMode;
  uint8_t  tileTintPct;    // slot-accent blend into tileColor, 0..30
  uint8_t  reserved;
  uint16_t valueColor;
  uint16_t labelColor;
  uint16_t secondaryColor;
  uint16_t tileColor;
};

// Display customization. Trimmed to the fields the reused chassis (display_ui,
// display_gauges, display_anim) actually reads - the Bambu printer/AMS/Tasmota
// fields are gone.
struct DisplaySettings {
  uint8_t  rotation;          // 0,1,2,3 (x90 degrees)
  uint16_t bgColor;           // background color
  uint16_t trackColor;        // inactive arc track color
  uint16_t progressBarColor;  // top LED progress bar fill color
  bool     animatedBar;       // shimmer effect on progress bar
  bool     smallLabels;       // use FONT_SMALL gauge labels instead of FONT_BODY
  bool     invertColors;      // invert display colors (panel-dependent fix)
  bool     cydPanelClassic;   // CYD only: plain Panel_ILI9341 (other hw revision)
  bool     pongClock;         // legacy Breakout selector, migrated to clockFace
  // Gauge full-scale ranges (arc maxima)
  uint16_t tempScaleMax;      // C (CPU/GPU temperature gauges)
  uint16_t powerScaleW;       // W (power gauge full-scale)
  uint8_t  gaugeSmoothing;    // arc easing: 0=Off 1=Slow 2=Normal 3=Fast
  uint16_t warnColor;         // temp-gauge over-threshold color
  uint8_t  warnThresholdPct;  // temp gauge turns warnColor at >= this % of scale; 0 = off
  // Clock idle screen
  uint16_t clockTimeColor;
  uint16_t clockDateColor;
  bool     hideClockDate;
  GaugeColors progress;       // progress arc colors
  GaugeColors gauge;          // generic default for temp/fan/power gauges
};

// Network settings (carried mostly intact - generic WiFi/IP/time config).
struct NetworkSettings {
  bool useDHCP;           // true = DHCP, false = static
  char staticIP[16];
  char gateway[16];
  char subnet[16];
  char dns[16];
  bool showIPAtStartup;   // show IP screen for a few seconds after WiFi connects
  uint8_t timezoneIndex;  // index into timezone database (for the idle clock)
  char timezoneStr[64];   // POSIX TZ string
  bool use24h;            // true = 24h format (default), false = 12h AM/PM
  uint8_t dateFormat;     // 0=DD.MM.YYYY .. (see formatter)
  bool mdnsEnabled;       // advertise <hostname>.local over mDNS
  char hostname[32];      // mDNS/host label, sanitized [a-z0-9-], default "statnode"
};

extern char wifiSSID[33];
extern char wifiPass[65];
extern uint8_t brightness;
extern DisplaySettings dispSettings;
extern NetworkSettings netSettings;
extern GaugeMapping gaugeMap;
extern GaugeLabels gaugeLabels;
extern BacklightSettings backlightSettings;
extern TouchSettings touchSettings;
extern LedSettings ledSettings;
extern ThemeSettings themeSettings;
extern uint8_t displayStyle;   // DisplayStyle value
extern uint8_t clockFace;      // ClockFace value
extern uint8_t sparkRedrawSec; // chart repaint cadence in seconds (own NVS key)
// Chart low pass, 0=Off 1=Light 2=Medium 3=Heavy. Rounds one-sample needles on
// the glass charts at the cost of shaving real peaks. Its OWN NVS key for the
// same reason as displayStyle: growing the "disp" blob resizes it and a size
// mismatch resets every display setting on the next OTA.
extern uint8_t chartSmoothing;
constexpr uint8_t CHART_SMOOTH_MAX = 3;

// Glass surface controls, all percentages, all their own NVS keys for the same
// reason chartSmoothing has one: growing the "disp" blob resizes it and a size
// mismatch resets every display setting on the next OTA.
//
// glassGlossPct  strength of the Aero highlight over the top of a pane.
// glassBowPct    how far that highlight narrows toward the pane's centre. 0 is
//                a bar of even brightness across the pane; high values pull it
//                into a centred oval, which on a short wide tile lands in the
//                gap between the label and the value and reads as a smudge.
// glassChartFill opacity of the wash under the chart line.
// Glass Duo is deliberately untouched by the two highlight controls - that face
// has no face highlight by design, only lit edges.
extern uint8_t glassGlossPct;
extern uint8_t glassBowPct;
extern uint8_t glassChartFillPct;
constexpr uint8_t GLASS_GLOSS_PCT_DEFAULT = 31;       // 80/255
constexpr uint8_t GLASS_BOW_PCT_DEFAULT = 16;         // 40/255
constexpr uint8_t GLASS_CHART_FILL_PCT_DEFAULT = 59;  // 150/255

// Glass colourway. Default keeps the ramp derived from the user's bgColor; the
// two Frosted presets replace both the backdrop ramp and the pane treatment
// with a fixed palette, so bgColor and the theme text colours stop applying
// while one is selected. Ported from the Home Assistant Frosted Glass themes
// (github.com/wessamlauf/homeassistant-frosted-glass-themes, MIT).
// Its OWN NVS key, same reason as the sliders above.
enum GlassThemeId : uint8_t {
  GLASS_THEME_DEFAULT = 0,
  GLASS_THEME_FROSTED_DARK = 1,
  GLASS_THEME_FROSTED_LIGHT = 2,
  GLASS_THEME_COUNT = 3
};
extern uint8_t glassTheme;

void loadSettings();
void saveSettings();
bool factoryResetSettings();
void defaultDisplaySettings(DisplaySettings& ds);
void defaultNetworkSettings(NetworkSettings& ns);
void defaultGaugeMapping(GaugeMapping& gm);
void defaultGaugeLabels(GaugeLabels& labels);
void defaultBacklightSettings(BacklightSettings& bs);
void defaultTouchSettings(TouchSettings& ts);
void defaultLedSettings(LedSettings& ls);
void saveLedSettings();
void defaultThemeSettings(ThemeSettings& ts);
void saveDisplayStyle();

// Keep custom labels ASCII-only because the bundled display fonts do not
// provide general UTF-8 glyph coverage. Leading/trailing spaces are removed.
void sanitizeGaugeLabel(const char* in, char* out, size_t outSize);
const char* gaugeDisplayLabel(uint8_t slotIndex, const char* fallback);

// RGB565 <-> HTML hex helpers (used by the web portal).
uint16_t htmlToRgb565(const char* hex);
void rgb565ToHtml(uint16_t color, char* buf);  // buf must be >= 8 chars

// Normalize a string into a valid mDNS/DNS label: lowercase, keep only
// [a-z0-9-], strip leading/trailing hyphens. Falls back to "statnode".
void sanitizeHostname(const char* in, char* out, size_t outSize);

#endif // SETTINGS_H
