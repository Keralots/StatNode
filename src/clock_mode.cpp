#include "clock_mode.h"
#include "display_ui.h"
#include "fonts.h"
#include "settings.h"
#include <time.h>

namespace {

constexpr int CLK_BASE_W = 32;
constexpr int CLK_BASE_H = 48;
constexpr int CLK_BASE_COLON = 12;
constexpr int DATE_FONT_H = 16;
constexpr int DATE_GAP = 14;

int prevMinute = -1;
bool prevColon = false;
int prevWidth = -1;
int prevHeight = -1;
bool prevUse24h = true;
bool prevHideDate = false;

class ClockWrite {
 public:
  ClockWrite() { tft.startWrite(); }
  ~ClockWrite() { tft.endWrite(); }
};

bool readLocalClock(struct tm& now) {
  if (getLocalTime(&now, 0)) return true;
  time_t raw = time(nullptr);
  if (raw < 1600000000UL) return false;
  localtime_r(&raw, &now);
  return true;
}

void formatDate(const struct tm& now, char* out, size_t outSize) {
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char* months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  const int day = now.tm_mday;
  const int mon = now.tm_mon + 1;
  const int year = now.tm_year + 1900;
  switch (netSettings.dateFormat) {
    case 1: snprintf(out, outSize, "%s %02d-%02d-%04d", days[now.tm_wday], day, mon, year); break;
    case 2: snprintf(out, outSize, "%s %02d/%02d/%04d", days[now.tm_wday], mon, day, year); break;
    case 3: snprintf(out, outSize, "%s %04d-%02d-%02d", days[now.tm_wday], year, mon, day); break;
    case 4: snprintf(out, outSize, "%s %d %s %04d", days[now.tm_wday], day, months[now.tm_mon], year); break;
    case 5: snprintf(out, outSize, "%s %s %d, %04d", days[now.tm_wday], months[now.tm_mon], day, year); break;
    default: snprintf(out, outSize, "%s %02d.%02d.%04d", days[now.tm_wday], day, mon, year); break;
  }
}

float clockScale(int screenW) {
  float scale = screenW >= 480 ? 2.0f : screenW >= 320 ? 1.5f : 1.0f;
  if (!netSettings.use24h) {
    setFont(tft, FONT_BODY);
    tft.setTextSize(1);
    const int suffixW = max(tft.textWidth("AM"), tft.textWidth("PM")) + 6;
    while (scale > 1.0f && (int)(4 * CLK_BASE_W * scale + CLK_BASE_COLON * scale) + suffixW > screenW - 4)
      scale -= 0.5f;
  }
  return scale;
}

int digitX(int index, int startX, int digitW, int colonW) {
  if (index < 2) return startX + index * digitW;
  if (index == 2) return startX + 2 * digitW;
  return startX + 2 * digitW + colonW + (index - 3) * digitW;
}

void renderFullClock(const struct tm& now, bool colonOn) {
  const int sw = (int)tft.width();
  const int sh = (int)tft.height();
  const uint16_t bg = dispSettings.bgColor;
  const float scale = clockScale(sw);
  const int digitW = (int)(CLK_BASE_W * scale);
  const int digitH = (int)(CLK_BASE_H * scale);
  const int colonW = (int)(CLK_BASE_COLON * scale);
  const int timeW = 4 * digitW + colonW;

  int suffixW = 0;
  int suffixTextW = 0;
  if (!netSettings.use24h) {
    setFont(tft, FONT_BODY);
    tft.setTextSize(1);
    suffixTextW = max(tft.textWidth("AM"), tft.textWidth("PM"));
    suffixW = suffixTextW + 6;
  }

  const int startX = (sw - timeW - suffixW) / 2;
  const int contentH = digitH + (dispSettings.hideClockDate ? 0 : DATE_GAP + DATE_FONT_H);
  const int timeY = (sh - contentH) / 2;
  int hour = now.tm_hour;
  char digits[5];
  if (netSettings.use24h) {
    digits[0] = '0' + hour / 10;
    digits[1] = '0' + hour % 10;
  } else {
    hour %= 12;
    if (hour == 0) hour = 12;
    digits[0] = hour >= 10 ? '1' : ' ';
    digits[1] = '0' + hour % 10;
  }
  digits[2] = ':';
  digits[3] = '0' + now.tm_min / 10;
  digits[4] = '0' + now.tm_min % 10;

  tft.fillScreen(bg);
  setFont(tft, FONT_7SEG);
  tft.setTextSize(scale);
  tft.setTextColor(dispSettings.clockTimeColor, bg);
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < 5; i++) {
    if (i == 2 && !colonOn) continue;
    tft.drawChar(digits[i], digitX(i, startX, digitW, colonW), timeY, 7);
  }

  if (!netSettings.use24h) {
    setFont(tft, FONT_BODY);
    tft.setTextSize(1);
    tft.setTextColor(dispSettings.clockDateColor, bg);
    tft.drawString(now.tm_hour < 12 ? "AM" : "PM", startX + timeW + 6,
                   timeY + digitH - DATE_FONT_H);
  }

  if (!dispSettings.hideClockDate) {
    char date[28];
    formatDate(now, date, sizeof(date));
    setFont(tft, FONT_BODY);
    tft.setTextSize(1);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(dispSettings.clockDateColor, bg);
    tft.drawString(date, sw / 2, timeY + digitH + DATE_GAP + DATE_FONT_H / 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void redrawColon(bool colonOn) {
  const int sw = (int)tft.width();
  const int sh = (int)tft.height();
  const float scale = clockScale(sw);
  const int digitW = (int)(CLK_BASE_W * scale);
  const int digitH = (int)(CLK_BASE_H * scale);
  const int colonW = (int)(CLK_BASE_COLON * scale);
  const int timeW = 4 * digitW + colonW;
  int suffixW = 0;
  if (!netSettings.use24h) {
    setFont(tft, FONT_BODY);
    tft.setTextSize(1);
    suffixW = max(tft.textWidth("AM"), tft.textWidth("PM")) + 6;
  }
  const int startX = (sw - timeW - suffixW) / 2;
  const int contentH = digitH + (dispSettings.hideClockDate ? 0 : DATE_GAP + DATE_FONT_H);
  const int timeY = (sh - contentH) / 2;
  const int x = digitX(2, startX, digitW, colonW);
  tft.fillRect(x, timeY, colonW, digitH, dispSettings.bgColor);
  if (colonOn) {
    setFont(tft, FONT_7SEG);
    tft.setTextSize(scale);
    tft.setTextColor(dispSettings.clockTimeColor, dispSettings.bgColor);
    tft.drawChar(':', x, timeY, 7);
  }
}

} // namespace

void resetClock() {
  prevMinute = -1;
  prevColon = false;
  prevWidth = -1;
  prevHeight = -1;
  prevUse24h = netSettings.use24h;
  prevHideDate = dispSettings.hideClockDate;
}

void drawClock() {
  struct tm now;
  if (!readLocalClock(now)) return;
  const bool colonOn = (millis() % 1000) < 500;
  const bool full = now.tm_min != prevMinute || (int)tft.width() != prevWidth ||
                    (int)tft.height() != prevHeight || netSettings.use24h != prevUse24h ||
                    dispSettings.hideClockDate != prevHideDate;
  ClockWrite write;
  if (full) {
    renderFullClock(now, colonOn);
    prevMinute = now.tm_min;
    prevWidth = (int)tft.width();
    prevHeight = (int)tft.height();
    prevUse24h = netSettings.use24h;
    prevHideDate = dispSettings.hideClockDate;
    prevColon = colonOn;
    markFrameDirty();
  } else if (colonOn != prevColon) {
    redrawColon(colonOn);
    prevColon = colonOn;
    markFrameDirty();
  }
}

void drawClockSnapshot() {
  struct tm now;
  if (!readLocalClock(now)) return;
  ClockWrite write;
  renderFullClock(now, (millis() % 1000) < 500);
  markFrameDirty();
}
