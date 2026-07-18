#include "clock_mario.h"
#include "display_ui.h"
#include "fonts.h"
#include "settings.h"
#include <time.h>

namespace {

constexpr int SPRITE_COLS = 12;
constexpr int SPRITE_ROWS = 16;
constexpr int UPDATE_MS = 35;
constexpr int TRIGGER_SECOND = 56;
constexpr int MAX_TARGETS = 4;
constexpr int MARIO_DIRTY_SIZE = 64;
constexpr unsigned long TIME_OVERRIDE_MAX_MS = 60000;

constexpr uint16_t MARIO_RED = 0xF800;
constexpr uint16_t MARIO_BROWN = 0x8200;
constexpr uint16_t MARIO_SKIN = 0xFD20;
constexpr uint16_t MARIO_BLUE = 0x001F;
constexpr uint16_t MARIO_WHITE = 0xFFFF;
constexpr uint16_t MARIO_GOLD = 0xFFE0;
constexpr uint16_t SCENE_SKY = 0x54DF;
constexpr uint16_t SCENE_OUTLINE = 0x0000;
constexpr uint16_t GROUND_TOP = SCENE_OUTLINE;
constexpr uint16_t GROUND_FILL = 0xFA00;
constexpr uint16_t GROUND_LINE = 0x8200;
constexpr uint16_t GROUND_LIGHT = 0xFDC0;
constexpr uint16_t SCENE_GREEN = 0x07E0;
constexpr uint16_t SCENE_DARK_GREEN = 0x0320;
constexpr uint16_t SCENE_LIGHT_GREEN = 0x87F0;
constexpr uint16_t SCENE_CLOUD = 0xFFFF;
constexpr uint16_t SCENE_CLOUD_SHADE = 0xBDF7;
constexpr uint16_t SCENE_BLOCK = 0xFBE0;
constexpr uint16_t SCENE_BLOCK_DARK = 0x8200;
constexpr uint16_t SCENE_BLOCK_LIGHT = 0xFFE0;

// Palette-indexed sprites are much smaller than RGB565 bitmaps. Each row is
// rendered as horizontal color runs, keeping the 240x240 animation inexpensive
// without allocating another full-screen sprite beside the portal capture.
static const char MARIO_STAND[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "011144441000", "331444414330", "333444443330", "003446430000",
  "002244220000", "022200222000", "022000022000", "220000002200"
};

static const char MARIO_WALK_A[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "011144441000", "331444414000", "333444443300", "003446430000",
  "002244220000", "002220022200", "022000000220", "220000000022"
};

static const char MARIO_WALK_B[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "001144441100", "003144414300", "033444443330", "003446430000",
  "002244220000", "002220022000", "000220022000", "002200000220"
};

static const char MARIO_WALK_C[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "000144441110", "000144414133", "003444443330", "003446430000",
  "002244220000", "022200002220", "220000000220", "002200000022"
};

static const char MARIO_WALK_D[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "001144441100", "003144414300", "033444443330", "003446430000",
  "002244220000", "002200022200", "002200022000", "000220220000"
};

static const char MARIO_JUMP[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "330144110330",
  "033144441330", "003444443300", "000444440000", "003446430000",
  "022244422200", "220244420220", "000222220000", "000220022000"
};

struct MarioLayout {
  int16_t w;
  int16_t h;
  float unit;
  float textScale;
  int16_t digitW;
  int16_t digitH;
  int16_t colonW;
  int16_t timeX;
  int16_t timeY;
  int16_t dateY;
  int16_t groundY;
  int16_t spriteScale;
  int16_t spriteW;
  int16_t spriteH;
};

enum MarioState : uint8_t {
  MARIO_IDLE,
  MARIO_WALKING,
  MARIO_JUMPING,
  MARIO_EXITING
};

struct CoinState {
  bool active;
  int16_t x;
  int16_t y;
  int16_t velocity;
  uint8_t frame;
};

MarioState state = MARIO_IDLE;
CoinState coin = {false, 0, 0, 0, 0};
char displayedDigits[5] = {0};
char targetDigits[5] = {0};
char renderedDigits[5] = {0};
uint8_t targets[MAX_TARGETS] = {0};
uint8_t targetCount = 0;
uint8_t targetIndex = 0;
int16_t digitOffset[5] = {0};
int16_t digitVelocity[5] = {0};
int16_t renderedOffset[5] = {0};
int16_t marioX = 0;
int16_t jumpOffset = 0;
int16_t jumpVelocity = 0;
bool digitHit = false;
bool facingRight = true;
uint8_t walkFrame = 0;
int32_t lastTriggerMinute = -1;
int32_t lastHeaderKey = -1;
int16_t previousMarioX = -1000;
int16_t previousMarioY = -1000;
int16_t previousCoinX = -1000;
int16_t previousCoinY = -1000;
bool previousColon = false;
bool timeOverridden = false;
bool initialized = false;
unsigned long lastTick = 0;
unsigned long timeOverrideStart = 0;

class MarioWrite {
 public:
  MarioWrite() { tft.startWrite(); }
  ~MarioWrite() { tft.endWrite(); }
};

int scaled(float value, float unit) {
  return max(1, (int)(value * unit + 0.5f));
}

MarioLayout currentLayout() {
  MarioLayout out;
  out.w = (int16_t)tft.width();
  out.h = (int16_t)tft.height();
  out.unit = min(out.w, out.h) / 240.0f;
  out.textScale = 1.5f * out.unit;
  out.digitW = (int16_t)scaled(32.0f, out.textScale);
  out.digitH = (int16_t)scaled(48.0f, out.textScale);
  out.colonW = (int16_t)scaled(12.0f, out.textScale);
  const int timeW = 4 * out.digitW + out.colonW;
  out.timeX = (int16_t)((out.w - timeW) / 2);
  out.timeY = (int16_t)(out.h - scaled(194.0f, out.unit));
  out.dateY = (int16_t)(out.h - scaled(232.0f, out.unit));
  out.groundY = (int16_t)(out.h - scaled(56.0f, out.unit));
  out.spriteScale = (int16_t)max(1, scaled(2.0f, out.unit));
  out.spriteW = SPRITE_COLS * out.spriteScale;
  out.spriteH = SPRITE_ROWS * out.spriteScale;
  return out;
}

bool readTime(struct tm& now, time_t& raw) {
  raw = time(nullptr);
  if (raw >= 1600000000UL) {
    localtime_r(&raw, &now);
    return true;
  }
  if (!getLocalTime(&now, 0)) return false;
  raw = mktime(&now);
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

void buildDigits(const struct tm& now, char out[5]) {
  int hour = now.tm_hour;
  if (netSettings.use24h) {
    out[0] = '0' + hour / 10;
    out[1] = '0' + hour % 10;
  } else {
    hour %= 12;
    if (hour == 0) hour = 12;
    out[0] = hour >= 10 ? '1' : ' ';
    out[1] = '0' + hour % 10;
  }
  out[2] = ':';
  out[3] = '0' + now.tm_min / 10;
  out[4] = '0' + now.tm_min % 10;
}

int digitX(const MarioLayout& layout, int index) {
  if (index < 2) return layout.timeX + index * layout.digitW;
  if (index == 2) return layout.timeX + 2 * layout.digitW;
  return layout.timeX + 2 * layout.digitW + layout.colonW +
         (index - 3) * layout.digitW;
}

uint16_t spriteColor(char index) {
  switch (index) {
    case '1': return MARIO_RED;
    case '2': return MARIO_BROWN;
    case '3': return MARIO_SKIN;
    case '4': return MARIO_BLUE;
    case '5': return MARIO_WHITE;
    case '6': return MARIO_GOLD;
    default: return SCENE_SKY;
  }
}

void drawMarioSpriteOn(lgfx::LGFXBase& gfx,
                       const char frame[SPRITE_ROWS][SPRITE_COLS + 1],
                       int x, int y, int scale, bool faceRight) {
  for (int row = 0; row < SPRITE_ROWS; row++) {
    int col = 0;
    while (col < SPRITE_COLS) {
      const int sourceCol = faceRight ? col : SPRITE_COLS - 1 - col;
      if ((char)pgm_read_byte(&frame[row][sourceCol]) == '0') {
        col++;
        continue;
      }
      int run = 1;
      while (col + run < SPRITE_COLS) {
        const int nextSource = faceRight ? col + run :
                               SPRITE_COLS - 1 - col - run;
        if ((char)pgm_read_byte(&frame[row][nextSource]) == '0') break;
        run++;
      }
      gfx.fillRect(x + col * scale - 1, y + row * scale - 1,
                   run * scale + 2, scale + 2, SCENE_OUTLINE);
      col += run;
    }
  }
  for (int row = 0; row < SPRITE_ROWS; row++) {
    int col = 0;
    while (col < SPRITE_COLS) {
      const int sourceCol = faceRight ? col : SPRITE_COLS - 1 - col;
      const char color = (char)pgm_read_byte(&frame[row][sourceCol]);
      if (color == '0') {
        col++;
        continue;
      }
      int run = 1;
      while (col + run < SPRITE_COLS) {
        const int nextSource = faceRight ? col + run : SPRITE_COLS - 1 - col - run;
        if ((char)pgm_read_byte(&frame[row][nextSource]) != color) break;
        run++;
      }
      gfx.fillRect(x + col * scale, y + row * scale,
                   run * scale, scale, spriteColor(color));
      col += run;
    }
  }
}

void drawMarioSprite(const char frame[SPRITE_ROWS][SPRITE_COLS + 1],
                     int x, int y, int scale, bool faceRight) {
  drawMarioSpriteOn(tft, frame, x, y, scale, faceRight);
}

void drawPixelHill(lgfx::LGFXBase& gfx, int centerX, int groundY,
                   int p, int rows, bool spots) {
  for (int row = 0; row < rows; row++) {
    const int levelsAbove = rows - row - 1;
    const int width = (2 + levelsAbove * 5 / 2) * p;
    gfx.fillRect(centerX - width / 2, groundY - (row + 1) * p,
                 width, p, SCENE_OUTLINE);
    if (width > 2 * p)
      gfx.fillRect(centerX - width / 2 + p, groundY - (row + 1) * p,
                   width - 2 * p, p, SCENE_GREEN);
  }
  if (spots) {
    gfx.fillRect(centerX - 4 * p, groundY - 5 * p,
                 2 * p, 2 * p, SCENE_DARK_GREEN);
    gfx.fillRect(centerX + 3 * p, groundY - 8 * p,
                 p, 2 * p, SCENE_OUTLINE);
  }
}

void drawPixelCloud(lgfx::LGFXBase& gfx, int x, int y, int p) {
  static const char cloud[6][13] PROGMEM = {
    "000111000000",
    "001222100000",
    "011222111100",
    "122222122210",
    "122222222221",
    "011333333110"
  };

  for (int row = 0; row < 6; row++) {
    int col = 0;
    while (col < 12) {
      const char color = (char)pgm_read_byte(&cloud[row][col]);
      if (color == '0') {
        col++;
        continue;
      }
      int run = 1;
      while (col + run < 12 &&
             (char)pgm_read_byte(&cloud[row][col + run]) == color)
        run++;
      const uint16_t rgb = color == '1' ? SCENE_OUTLINE :
                           color == '2' ? SCENE_CLOUD : SCENE_CLOUD_SHADE;
      gfx.fillRect(x + col * p, y + row * p, run * p, p, rgb);
      col += run;
    }
  }
}

void drawCloudsOn(lgfx::LGFXBase& gfx, const MarioLayout& layout,
                  int originX, int originY) {
  const int p = scaled(2.0f, layout.unit);
  const int cloudW = 12 * p;
  const int edge = scaled(4.0f, layout.unit);
  const int sideY = scaled(18.0f, layout.unit) - originY;
  drawPixelCloud(gfx, edge - originX, sideY, p);
  drawPixelCloud(gfx, layout.w - cloudW - edge - originX, sideY, p);
  if (dispSettings.hideClockDate) {
    drawPixelCloud(gfx, (layout.w - cloudW) / 2 - originX,
                   scaled(4.0f, layout.unit) - originY, p);
  }
}

void drawBrickBlock(lgfx::LGFXBase& gfx, int x, int y, int p) {
  const int size = 8 * p;
  gfx.fillRect(x, y, size, size, SCENE_BLOCK);
  gfx.drawRect(x, y, size, size, SCENE_OUTLINE);
  gfx.drawFastHLine(x + 1, y + 4 * p, size - 2, SCENE_BLOCK_DARK);
  gfx.drawFastVLine(x + 4 * p, y + 1, 4 * p - 1, SCENE_BLOCK_DARK);
  gfx.drawFastVLine(x + 2 * p, y + 4 * p, 4 * p - 1,
                    SCENE_BLOCK_DARK);
  gfx.fillRect(x + p, y + p, p, p, SCENE_BLOCK_LIGHT);
}

void drawQuestionBlock(lgfx::LGFXBase& gfx, int x, int y, int p) {
  const int size = 8 * p;
  gfx.fillRect(x, y, size, size, SCENE_BLOCK_LIGHT);
  gfx.drawRect(x, y, size, size, SCENE_OUTLINE);
  gfx.fillRect(x + p, y + p, p, p, MARIO_WHITE);
  gfx.fillRect(x + 3 * p, y + 2 * p, 3 * p, p, SCENE_BLOCK_DARK);
  gfx.fillRect(x + 5 * p, y + 3 * p, p, 2 * p, SCENE_BLOCK_DARK);
  gfx.fillRect(x + 4 * p, y + 4 * p, p, 2 * p, SCENE_BLOCK_DARK);
  gfx.fillRect(x + 4 * p, y + 6 * p, p, p, SCENE_BLOCK_DARK);
}

void drawPixelPipe(lgfx::LGFXBase& gfx, int x, int groundY, int p) {
  const int bodyY = groundY - 22 * p;
  gfx.fillRect(x, bodyY + 4 * p, 8 * p, 18 * p, SCENE_OUTLINE);
  gfx.fillRect(x + p, bodyY + 5 * p, 6 * p, 17 * p, SCENE_GREEN);
  gfx.fillRect(x + 2 * p, bodyY + 5 * p, p, 17 * p, SCENE_LIGHT_GREEN);
  gfx.fillRect(x - p, bodyY, 10 * p, 5 * p, SCENE_OUTLINE);
  gfx.fillRect(x, bodyY + p, 8 * p, 3 * p, SCENE_GREEN);
  gfx.fillRect(x + p, bodyY + p, p, 2 * p, SCENE_LIGHT_GREEN);
  for (int y = bodyY + 6 * p; y < groundY; y += 2 * p)
    gfx.fillRect(x + 6 * p, y, p, p, SCENE_DARK_GREEN);
}

void drawBackgroundSceneryOn(lgfx::LGFXBase& gfx, const MarioLayout& layout,
                             int originX, int originY) {
  const int p = scaled(2.0f, layout.unit);
  const int blockSize = 8 * p;
  const int groupW = 5 * blockSize;
  const int blockX = layout.w - groupW - scaled(12.0f, layout.unit) - originX;
  const int blockY = layout.groundY - scaled(50.0f, layout.unit) - originY;
  const int groundY = layout.groundY - originY;

  drawCloudsOn(gfx, layout, originX, originY);
  drawPixelHill(gfx, scaled(26.0f, layout.unit) - originX,
                groundY, p, 13, true);
  drawPixelPipe(gfx, scaled(72.0f, layout.unit) - originX, groundY, p);
  drawPixelHill(gfx, layout.w - scaled(82.0f, layout.unit) - originX,
                groundY, p, 7, false);
  drawBrickBlock(gfx, blockX, blockY, p);
  drawBrickBlock(gfx, blockX + blockSize, blockY, p);
  drawQuestionBlock(gfx, blockX + 2 * blockSize, blockY, p);
  drawBrickBlock(gfx, blockX + 3 * blockSize, blockY, p);
  drawBrickBlock(gfx, blockX + 4 * blockSize, blockY, p);
}

void drawBackgroundScenery(const MarioLayout& layout) {
  drawBackgroundSceneryOn(tft, layout, 0, 0);
}

void drawGroundOn(lgfx::LGFXBase& gfx, const MarioLayout& layout,
                  int originX, int originY) {
  const int tile = scaled(12.0f, layout.unit);
  gfx.fillRect(-originX, layout.groundY - originY,
               layout.w, layout.h - layout.groundY, GROUND_FILL);
  gfx.drawFastHLine(-originX, layout.groundY - originY,
                    layout.w, GROUND_TOP);
  for (int y = layout.groundY + tile; y < layout.h; y += tile) {
    gfx.drawFastHLine(-originX, y - originY, layout.w, GROUND_LINE);
    if (y + 1 < layout.h)
      gfx.drawFastHLine(-originX, y + 1 - originY, layout.w, GROUND_LIGHT);
  }
  int row = 0;
  for (int y = layout.groundY; y < layout.h; y += tile, row++) {
    const int offset = (row & 1) ? tile / 2 : 0;
    for (int x = offset; x < layout.w; x += tile) {
      gfx.drawFastVLine(x - originX, y - originY,
                       min(tile, layout.h - y), GROUND_LINE);
      const int highlightW = min(tile - 4, layout.w - x - 2);
      if (highlightW > 0 && y + 2 < layout.h)
        gfx.drawFastHLine(x + 2 - originX, y + 2 - originY,
                         highlightW, GROUND_LIGHT);
    }
  }
}

void drawGround(const MarioLayout& layout) {
  drawGroundOn(tft, layout, 0, 0);
}

void drawHeader(const MarioLayout& layout, const struct tm& now, bool clear) {
  const int headerH = scaled(20.0f, layout.unit);
  if (clear) {
    tft.fillRect(0, max(0, layout.dateY - 2), layout.w, headerH + 4,
                 SCENE_SKY);
    drawCloudsOn(tft, layout, 0, 0);
  }
  setFont(tft, FONT_BODY);
  tft.setTextSize(layout.unit >= 1.75f ? 2.0f : 1.0f);
  tft.setTextColor(dispSettings.clockDateColor, SCENE_SKY);
  if (!dispSettings.hideClockDate) {
    char date[28];
    formatDate(now, date, sizeof(date));
    tft.setTextDatum(TC_DATUM);
    tft.drawString(date, layout.w / 2, layout.dateY);
  }
  if (!netSettings.use24h) {
    tft.setTextDatum(TR_DATUM);
    tft.drawString(now.tm_hour < 12 ? "AM" : "PM",
                   layout.w - scaled(4.0f, layout.unit), layout.dateY);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawDigits(const MarioLayout& layout, const char digits[5], bool colonOn,
                 const int16_t offsets[5], bool clear) {
  const int bouncePad = scaled(14.0f, layout.unit);
  setFont(tft, FONT_7SEG);
  tft.setTextSize(layout.textScale);
  tft.setTextColor(dispSettings.clockTimeColor, SCENE_SKY);
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < 5; i++) {
    const int x = digitX(layout, i);
    const int cellW = i == 2 ? layout.colonW : layout.digitW;
    if (clear)
      tft.fillRect(x, layout.timeY - bouncePad, cellW + 2,
                   layout.digitH + 2 * bouncePad, SCENE_SKY);
    if (digits[i] == ' ' || (i == 2 && !colonOn)) continue;
    tft.drawChar(digits[i], x, layout.timeY + offsets[i], 7);
  }
}

void drawDirtyDigits(const MarioLayout& layout, const char digits[5],
                     bool colonOn, const int16_t offsets[5], uint8_t forceMask) {
  const int bouncePad = scaled(14.0f, layout.unit);
  setFont(tft, FONT_7SEG);
  tft.setTextSize(layout.textScale);
  tft.setTextColor(dispSettings.clockTimeColor, SCENE_SKY);
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < 5; i++) {
    const bool dirty = (forceMask & (1u << i)) != 0 ||
                       renderedDigits[i] != digits[i] ||
                       renderedOffset[i] != offsets[i] ||
                       (i == 2 && previousColon != colonOn);
    if (!dirty) continue;
    const int x = digitX(layout, i);
    const int cellW = i == 2 ? layout.colonW : layout.digitW;
    tft.fillRect(x, layout.timeY - bouncePad, cellW + 2,
                 layout.digitH + 2 * bouncePad, SCENE_SKY);
    if (digits[i] != ' ' && (i != 2 || colonOn))
      tft.drawChar(digits[i], x, layout.timeY + offsets[i], 7);
    renderedDigits[i] = digits[i];
    renderedOffset[i] = offsets[i];
  }
  previousColon = colonOn;
}

void drawColon(const MarioLayout& layout, bool colonOn) {
  const int x = digitX(layout, 2);
  tft.fillRect(x, layout.timeY, layout.colonW + 2,
               layout.digitH, SCENE_SKY);
  if (!colonOn) return;
  setFont(tft, FONT_7SEG);
  tft.setTextSize(layout.textScale);
  tft.setTextColor(dispSettings.clockTimeColor, SCENE_SKY);
  tft.setTextDatum(TL_DATUM);
  tft.drawChar(':', x, layout.timeY, 7);
}

void restoreBackgroundRect(const MarioLayout& layout,
                           int x, int y, int w, int h) {
  const int left = max(0, x);
  const int top = max(0, y);
  const int right = min((int)tft.width(), x + w);
  const int bottom = min((int)tft.height(), y + h);
  if (right <= left || bottom <= top) return;

  int32_t oldX, oldY, oldW, oldH;
  tft.getClipRect(&oldX, &oldY, &oldW, &oldH);
  tft.setClipRect(left, top, right - left, bottom - top);
  tft.fillRect(left, top, right - left, bottom - top, SCENE_SKY);
  drawBackgroundScenery(layout);
  if (top <= layout.groundY && bottom > layout.groundY)
    tft.drawFastHLine(0, layout.groundY, layout.w, GROUND_TOP);
  tft.setClipRect(oldX, oldY, oldW, oldH);
}

bool rectanglesOverlap(int ax, int ay, int aw, int ah,
                       int bx, int by, int bw, int bh) {
  return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

uint8_t digitMaskForRect(const MarioLayout& layout, int x, int y, int w, int h) {
  const int bouncePad = scaled(14.0f, layout.unit);
  uint8_t mask = 0;
  for (int i = 0; i < 5; i++) {
    const int cellW = i == 2 ? layout.colonW : layout.digitW;
    if (rectanglesOverlap(x, y, w, h, digitX(layout, i),
                          layout.timeY - bouncePad, cellW + 2,
                          layout.digitH + 2 * bouncePad))
      mask |= (1u << i);
  }
  return mask;
}

int coinWide(const MarioLayout& layout) {
  return scaled(12.0f, layout.unit);
}

int coinHeight(const MarioLayout& layout) {
  return scaled(16.0f, layout.unit);
}

uint8_t erasePreviousDynamic(const MarioLayout& layout) {
  uint8_t restoreMask = 0;
  if (previousMarioX > -900) {
    restoreMask |= digitMaskForRect(
      layout, previousMarioX - 1, previousMarioY - 1,
      layout.spriteW + 2, layout.spriteH + 2);
    restoreBackgroundRect(layout, previousMarioX - 1, previousMarioY - 1,
                          layout.spriteW + 2, layout.spriteH + 2);
  }
  if (previousCoinX > -900) {
    const int coinW = coinWide(layout);
    const int coinH = coinHeight(layout);
    restoreMask |= digitMaskForRect(
      layout, previousCoinX - 1, previousCoinY - 1, coinW + 2, coinH + 2);
    restoreBackgroundRect(layout, previousCoinX - 1, previousCoinY - 1,
                          coinW + 2, coinH + 2);
  }
  previousMarioX = previousMarioY = -1000;
  previousCoinX = previousCoinY = -1000;
  return restoreMask;
}

uint8_t erasePreviousCoin(const MarioLayout& layout) {
  if (previousCoinX <= -900) return 0;
  const int coinW = coinWide(layout);
  const int coinH = coinHeight(layout);
  const uint8_t restoreMask = digitMaskForRect(
    layout, previousCoinX - 1, previousCoinY - 1, coinW + 2, coinH + 2);
  restoreBackgroundRect(layout, previousCoinX - 1, previousCoinY - 1,
                        coinW + 2, coinH + 2);
  previousCoinX = previousCoinY = -1000;
  return restoreMask;
}

void paintCoinOn(lgfx::LGFXBase& gfx, const MarioLayout& layout,
                 int originX, int originY) {
  if (!coin.active) return;
  const int wide = coinWide(layout);
  const int narrow = scaled(4.0f, layout.unit);
  const int height = coinHeight(layout);
  const int width = ((coin.frame / 3) & 1) ? narrow : wide;
  const int x = coin.x + (wide - width) / 2 - originX;
  const int y = coin.y - originY;
  gfx.fillRoundRect(x, y, width, height, max(1, width / 2), MARIO_GOLD);
  if (width > narrow)
    gfx.drawFastVLine(x + width / 2, y + 2,
                      max(1, height - 4), MARIO_WHITE);
}

void drawCoin(const MarioLayout& layout) {
  if (!coin.active) return;
  paintCoinOn(tft, layout, 0, 0);
  previousCoinX = coin.x;
  previousCoinY = coin.y;
}

void spawnCoin(const MarioLayout& layout, int index) {
  coin.active = true;
  coin.x = digitX(layout, index) + layout.digitW / 2 - coinWide(layout) / 2;
  coin.y = layout.timeY + scaled(4.0f, layout.unit);
  coin.velocity = -scaled(8.0f, layout.unit);
  coin.frame = 0;
}

void updateCoin(const MarioLayout& layout) {
  if (!coin.active) return;
  coin.y += coin.velocity;
  coin.velocity += max(1, layout.spriteScale / 2);
  coin.frame++;
  if (coin.frame > 20 || coin.y > layout.timeY + scaled(4.0f, layout.unit))
    coin.active = false;
}

void updateDigitBounce(const MarioLayout& layout) {
  const int gravity = max(1, layout.spriteScale / 2);
  for (int i = 0; i < 5; i++) {
    if (digitOffset[i] == 0 && digitVelocity[i] == 0) continue;
    digitOffset[i] += digitVelocity[i];
    digitVelocity[i] += gravity;
    if (digitOffset[i] >= 0) {
      digitOffset[i] = 0;
      digitVelocity[i] = 0;
    }
  }
}

void beginMinuteChange(const MarioLayout& layout, const struct tm& next) {
  buildDigits(next, targetDigits);
  targetCount = 0;
  static const uint8_t digitIndices[MAX_TARGETS] = {0, 1, 3, 4};
  for (uint8_t index : digitIndices) {
    if (displayedDigits[index] != targetDigits[index])
      targets[targetCount++] = index;
  }
  if (targetCount == 0) return;
  targetIndex = 0;
  marioX = -layout.spriteW;
  jumpOffset = 0;
  jumpVelocity = 0;
  digitHit = false;
  facingRight = true;
  walkFrame = 0;
  state = MARIO_WALKING;
}

int targetMarioX(const MarioLayout& layout) {
  const int index = targets[targetIndex];
  return digitX(layout, index) + layout.digitW / 2 - layout.spriteW / 2;
}

void advanceAnimation(const MarioLayout& layout) {
  updateDigitBounce(layout);
  updateCoin(layout);
  const int walkStep = max(3, layout.spriteScale * 5 / 2);
  const int gravity = max(1, layout.spriteScale / 2);

  switch (state) {
    case MARIO_WALKING: {
      const int target = targetMarioX(layout);
      const int delta = target - marioX;
      if (abs(delta) <= walkStep) {
        marioX = target;
        state = MARIO_JUMPING;
        jumpOffset = 0;
        jumpVelocity = -max(8, layout.spriteScale * 9 / 2);
        digitHit = false;
      } else {
        facingRight = delta > 0;
        marioX += facingRight ? walkStep : -walkStep;
        walkFrame = (walkFrame + 1) % 8;
      }
      break;
    }
    case MARIO_JUMPING: {
      jumpOffset += jumpVelocity;
      jumpVelocity += gravity;
      const int spriteTop = layout.groundY - layout.spriteH + jumpOffset;
      const int digitBottom = layout.timeY + layout.digitH;
      if (!digitHit && spriteTop <= digitBottom + scaled(3.0f, layout.unit)) {
        digitHit = true;
        const int index = targets[targetIndex];
        displayedDigits[index] = targetDigits[index];
        digitOffset[index] = -scaled(12.0f, layout.unit);
        digitVelocity[index] = gravity;
        if (!timeOverridden) timeOverrideStart = millis();
        timeOverridden = true;
        spawnCoin(layout, index);
        jumpVelocity = max(3, layout.spriteScale * 2);
      }
      if (jumpOffset >= 0 && jumpVelocity > 0) {
        jumpOffset = 0;
        jumpVelocity = 0;
        targetIndex++;
        if (targetIndex < targetCount) {
          facingRight = targetMarioX(layout) >= marioX;
          state = MARIO_WALKING;
        } else {
          facingRight = true;
          state = MARIO_EXITING;
        }
      }
      break;
    }
    case MARIO_EXITING:
      marioX += walkStep;
      walkFrame = (walkFrame + 1) % 8;
      if (marioX > layout.w + layout.spriteW) state = MARIO_IDLE;
      break;
    default:
      break;
  }
}

const char (*activeMarioFrame())[SPRITE_COLS + 1] {
  const char (*frame)[SPRITE_COLS + 1] = MARIO_STAND;
  if (state == MARIO_JUMPING) frame = MARIO_JUMP;
  else {
    switch ((walkFrame / 2) & 3) {
      case 0: frame = MARIO_WALK_A; break;
      case 1: frame = MARIO_WALK_B; break;
      case 2: frame = MARIO_WALK_C; break;
      default: frame = MARIO_WALK_D; break;
    }
  }
  return frame;
}

void paintDigitsOn(lgfx::LGFXBase& gfx, const MarioLayout& layout,
                   int originX, int originY, bool colonOn) {
  gfx.setTextFont(7);
  gfx.setTextSize(layout.textScale);
  gfx.setTextColor(dispSettings.clockTimeColor, SCENE_SKY);
  gfx.setTextDatum(TL_DATUM);
  for (int i = 0; i < 5; i++) {
    if (displayedDigits[i] == ' ' || (i == 2 && !colonOn)) continue;
    gfx.drawChar(displayedDigits[i], digitX(layout, i) - originX,
                 layout.timeY + digitOffset[i] - originY, 7);
  }
}

bool composeMarioTransition(const MarioLayout& layout,
                            int oldX, int oldY, bool newVisible,
                            int newX, int newY,
                            const char newFrame[SPRITE_ROWS][SPRITE_COLS + 1],
                            bool colonOn) {
  const bool oldVisible = oldX > -900;
  if (!oldVisible && !newVisible) return true;

  int left = layout.w;
  int top = layout.h;
  int right = 0;
  int bottom = 0;
  auto includeSprite = [&](int x, int y) {
    left = min(left, x - 1);
    top = min(top, y - 1);
    right = max(right, x + layout.spriteW + 1);
    bottom = max(bottom, y + layout.spriteH + 1);
  };
  if (oldVisible) includeSprite(oldX, oldY);
  if (newVisible) includeSprite(newX, newY);

  if (right <= 0 || bottom <= 0 || left >= layout.w || top >= layout.h)
    return true;
  left = max(0, left);
  top = max(0, top);
  right = min((int)layout.w, right);
  bottom = min((int)layout.h, bottom);
  if (right - left > MARIO_DIRTY_SIZE || bottom - top > MARIO_DIRTY_SIZE)
    return false;
  // The full 64x64 sprite is pushed each time, so keep the window entirely
  // on-panel. A partially clipped push smears columns at the right edge
  // during Mario's exit walk (seen with LovyanGFX 1.2.25 on the C3).
  if (layout.w >= MARIO_DIRTY_SIZE)
    left = min(left, (int)(layout.w - MARIO_DIRTY_SIZE));
  if (layout.h >= MARIO_DIRTY_SIZE)
    top = min(top, (int)(layout.h - MARIO_DIRTY_SIZE));

  static lgfx::LGFX_Sprite dirtySprite;
  static bool spriteReady = false;
  if (!spriteReady) {
    dirtySprite.setColorDepth(16);
    spriteReady = dirtySprite.createSprite(MARIO_DIRTY_SIZE, MARIO_DIRTY_SIZE);
    if (!spriteReady) return false;
  }

  dirtySprite.fillSprite(SCENE_SKY);
  drawBackgroundSceneryOn(dirtySprite, layout, left, top);
  drawGroundOn(dirtySprite, layout, left, top);
  paintDigitsOn(dirtySprite, layout, left, top, colonOn);
  paintCoinOn(dirtySprite, layout, left, top);
  if (newVisible)
    drawMarioSpriteOn(dirtySprite, newFrame, newX - left, newY - top,
                      layout.spriteScale, facingRight);
  dirtySprite.pushSprite(tft_ptr, left, top);
  tft.waitDMA();
  return true;
}

void drawAnimatedMario(const MarioLayout& layout) {
  if (state == MARIO_IDLE) return;
  const int y = layout.groundY - layout.spriteH + jumpOffset;
  const char (*frame)[SPRITE_COLS + 1] = activeMarioFrame();
  drawMarioSprite(frame, marioX, y, layout.spriteScale, facingRight);
  previousMarioX = marioX;
  previousMarioY = y;
}

int32_t headerKey(const struct tm& now) {
  const int pm = !netSettings.use24h && now.tm_hour >= 12 ? 1 : 0;
  return ((now.tm_year * 367 + now.tm_yday) * 2) + pm;
}

void initialize(const MarioLayout& layout, const struct tm& now) {
  tft.fillScreen(SCENE_SKY);
  drawBackgroundScenery(layout);
  drawGround(layout);
  buildDigits(now, displayedDigits);
  memset(targetDigits, 0, sizeof(targetDigits));
  memset(digitOffset, 0, sizeof(digitOffset));
  memset(digitVelocity, 0, sizeof(digitVelocity));
  const bool colonOn = (millis() % 1000) < 500;
  drawHeader(layout, now, false);
  drawDigits(layout, displayedDigits, colonOn, digitOffset, false);
  memcpy(renderedDigits, displayedDigits, sizeof(renderedDigits));
  memcpy(renderedOffset, digitOffset, sizeof(renderedOffset));
  previousColon = colonOn;
  lastHeaderKey = headerKey(now);
  state = MARIO_IDLE;
  coin.active = false;
  timeOverridden = false;
  previousMarioX = previousMarioY = -1000;
  previousCoinX = previousCoinY = -1000;
  initialized = true;
}

void drawSnapshotScene(const MarioLayout& layout, const struct tm& now) {
  char digits[5];
  int16_t offsets[5] = {0};
  buildDigits(now, digits);
  tft.fillScreen(SCENE_SKY);
  drawBackgroundScenery(layout);
  drawGround(layout);
  drawHeader(layout, now, false);
  drawDigits(layout, digits, (millis() % 1000) < 500, offsets, false);
  const int x = scaled(14.0f, layout.unit);
  const int y = layout.groundY - layout.spriteH;
  drawMarioSprite(MARIO_STAND, x, y, layout.spriteScale, true);
}

} // namespace

void resetMarioClock() {
  initialized = false;
  lastTick = 0;
  state = MARIO_IDLE;
  coin.active = false;
  timeOverridden = false;
  timeOverrideStart = 0;
  lastTriggerMinute = -1;
  previousMarioX = previousMarioY = -1000;
  previousCoinX = previousCoinY = -1000;
}

void tickMarioClock() {
  const unsigned long nowMs = millis();
  if (initialized && nowMs - lastTick < UPDATE_MS) return;
  lastTick = nowMs;

  struct tm now;
  time_t raw;
  if (!readTime(now, raw)) return;
  const MarioLayout layout = currentLayout();
  MarioWrite write;

  if (!initialized) {
    initialize(layout, now);
    markFrameDirty();
    return;
  }

  const bool colonOn = (nowMs % 1000) < 500;
  char currentDigits[5];
  buildDigits(now, currentDigits);
  bool changed = false;
  if (headerKey(now) != lastHeaderKey) {
    drawHeader(layout, now, true);
    lastHeaderKey = headerKey(now);
    changed = true;
  }

  if (timeOverridden) {
    const bool timeCaughtUp =
      state == MARIO_IDLE &&
      memcmp(displayedDigits, currentDigits, sizeof(displayedDigits)) == 0;
    const bool timedOut = nowMs - timeOverrideStart > TIME_OVERRIDE_MAX_MS;
    if (timeCaughtUp) {
      timeOverridden = false;
    } else if (timedOut) {
      const uint8_t restoreMask = erasePreviousDynamic(layout);
      state = MARIO_IDLE;
      coin.active = false;
      jumpOffset = 0;
      jumpVelocity = 0;
      memset(digitOffset, 0, sizeof(digitOffset));
      memset(digitVelocity, 0, sizeof(digitVelocity));
      memcpy(displayedDigits, currentDigits, sizeof(displayedDigits));
      timeOverridden = false;
      drawDirtyDigits(layout, displayedDigits, colonOn, digitOffset,
                      restoreMask | 0x1F);
      changed = true;
    }
  }

  if (state == MARIO_IDLE) {
    if (!timeOverridden &&
        memcmp(displayedDigits, currentDigits, sizeof(displayedDigits)) != 0) {
      memcpy(displayedDigits, currentDigits, sizeof(displayedDigits));
      drawDirtyDigits(layout, displayedDigits, colonOn, digitOffset, 0x1F);
      changed = true;
    }

    const int32_t minuteKey = (int32_t)(raw / 60);
    if (now.tm_sec >= TRIGGER_SECOND && minuteKey != lastTriggerMinute) {
      lastTriggerMinute = minuteKey;
      const time_t nextRaw = raw + 60;
      struct tm next;
      localtime_r(&nextRaw, &next);
      beginMinuteChange(layout, next);
    }
  }

  if (state != MARIO_IDLE || coin.active) {
    const int oldMarioX = previousMarioX;
    const int oldMarioY = previousMarioY;
    const uint8_t restoreMask = erasePreviousCoin(layout);
    advanceAnimation(layout);
    drawDirtyDigits(layout, displayedDigits, colonOn, digitOffset, restoreMask);
    drawCoin(layout);
    const bool newMarioVisible = state != MARIO_IDLE;
    const int newMarioY = layout.groundY - layout.spriteH + jumpOffset;
    const char (*newFrame)[SPRITE_COLS + 1] = activeMarioFrame();
    if (composeMarioTransition(layout, oldMarioX, oldMarioY,
                               newMarioVisible, marioX, newMarioY,
                               newFrame, colonOn)) {
      if (newMarioVisible) {
        previousMarioX = marioX;
        previousMarioY = newMarioY;
      } else {
        previousMarioX = previousMarioY = -1000;
      }
    } else {
      uint8_t marioMask = 0;
      if (oldMarioX > -900) {
        marioMask = digitMaskForRect(
          layout, oldMarioX - 1, oldMarioY - 1,
          layout.spriteW + 2, layout.spriteH + 2);
        restoreBackgroundRect(layout, oldMarioX - 1, oldMarioY - 1,
                              layout.spriteW + 2, layout.spriteH + 2);
      }
      drawDirtyDigits(layout, displayedDigits, colonOn, digitOffset, marioMask);
      drawCoin(layout);
      drawAnimatedMario(layout);
      if (!newMarioVisible)
        previousMarioX = previousMarioY = -1000;
    }
    changed = true;
  } else if (colonOn != previousColon) {
    drawColon(layout, colonOn);
    previousColon = colonOn;
    changed = true;
  }

  if (changed) markFrameDirty();
}

void drawMarioClockSnapshot() {
  struct tm now;
  time_t raw;
  if (!readTime(now, raw)) return;
  const MarioLayout layout = currentLayout();
  MarioWrite write;
  drawSnapshotScene(layout, now);
  markFrameDirty();
}
