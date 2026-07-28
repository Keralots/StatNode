#include "clock_runner.h"
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
constexpr int RUNNER_DIRTY_SIZE = 64;
constexpr unsigned long TIME_OVERRIDE_MAX_MS = 60000;

constexpr uint16_t RUNNER_RED = 0xF800;
constexpr uint16_t RUNNER_BROWN = 0x8200;
constexpr uint16_t RUNNER_SKIN = 0xFD20;
constexpr uint16_t RUNNER_BLUE = 0x001F;
constexpr uint16_t RUNNER_WHITE = 0xFFFF;
constexpr uint16_t RUNNER_GOLD = 0xFFE0;
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
constexpr uint16_t SCENE_BLOCK = 0xFBE0;
constexpr uint16_t SCENE_BLOCK_DARK = 0x8200;
constexpr uint16_t SCENE_BLOCK_LIGHT = 0xFFE0;
// Sky-tinted cloud underside reads as bounce light where the old flat grey
// read as dirt. The three character colors are the only other additions.
constexpr uint16_t SCENE_CLOUD_SKY = 0xAE1F;
constexpr uint16_t CHAR_TAN = 0xFD8B;
constexpr uint16_t SHELL_GREEN = 0x0540;
constexpr uint16_t SHELL_DARK = 0x0200;

// Palette-indexed sprites are much smaller than RGB565 bitmaps. Each row is
// rendered as horizontal color runs, keeping the 240x240 animation inexpensive
// without allocating another full-screen sprite beside the portal capture.
// Stand and jump are transcribed pixel for pixel from the reference poses and
// remapped to this project's palette: the reference's brown shirt becomes red,
// its red overalls become blue.
static const char RUNNER_STAND[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111111000", "001111111110", "002223323000", "023233323330",
  "023223332333", "022333322220", "000333333300", "000141110000",
  "011141141110", "111141141111", "331464464133", "333444444333",
  "334444444433", "004440044400", "022200002220", "222200002222"
};

static const char RUNNER_WALK_A[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "011144441000", "331444414000", "333444443300", "003446430000",
  "002244220000", "004440044400", "022000000220", "220000000022"
};

static const char RUNNER_WALK_B[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "001144441100", "003144414300", "033444443330", "003446430000",
  "002244220000", "004440044000", "000220022000", "002200000220"
};

static const char RUNNER_WALK_C[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "000144441110", "000144414133", "003444443330", "003446430000",
  "002244220000", "044400004440", "220000000220", "002200000022"
};

static const char RUNNER_WALK_D[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000111110000", "001111111100", "002223330000", "022323333000",
  "022332333300", "002233330000", "000333300000", "001144110000",
  "001144441100", "003144414300", "033444443330", "003446430000",
  "002244220000", "004400044400", "002200022000", "000220220000"
};

// The jump pose occupies rows 2..13 of its cell rather than filling it, so
// drawn at the same y as every other frame the feet lift two pixels and the
// head drops two. That is the tuck the pose wants, with no offset table.
static const char RUNNER_JUMP[SPRITE_ROWS][SPRITE_COLS + 1] PROGMEM = {
  "000000000000", "000000000000", "000000000033", "000001110033",
  "000011111133", "000233323311", "000232332331", "000003333300",
  "001114114100", "011111411401", "330044464611", "032044444411",
  "022444440000", "020444000000", "000000000000", "000000000000"
};

// --- scenery sprites -------------------------------------------------------
// Clouds use their own three-entry palette (outline / body / underside); the
// characters use the shared sprite palette below.
constexpr int CLOUD_NEAR_COLS = 20;
constexpr int CLOUD_NEAR_ROWS = 8;
constexpr int CLOUD_FAR_COLS = 12;
constexpr int CLOUD_FAR_ROWS = 6;

static const char CLOUD_NEAR[CLOUD_NEAR_ROWS][CLOUD_NEAR_COLS + 1] PROGMEM = {
  "00000000111100000000", "00000001222210000000",
  "00110001222210001100", "01221012222221012210",
  "12222122222222122221", "12222222222222222221",
  "13333333333333333331", "01111111111111111110"
};

static const char CLOUD_FAR[CLOUD_FAR_ROWS][CLOUD_FAR_COLS + 1] PROGMEM = {
  "000001111000", "001101222100", "012212222210",
  "122222222221", "133333333331", "011111111110"
};

constexpr int CHAR_COLS = 12;
constexpr int WALKER_ROWS = 12;
constexpr int SHELL_ROWS = 16;

static const char WALKER_FRAMES[2][WALKER_ROWS][CHAR_COLS + 1] PROGMEM = {
  {
    "000077770000", "000722227000", "007222222700", "072222222270",
    "072772277270", "725752257527", "725752257527", "072552255270",
    "077222222770", "788888888887", "778888888877", "770000000077"
  },
  {
    "000077770000", "000722227000", "007222222700", "072222222270",
    "072772277270", "725752257527", "725752257527", "072552255270",
    "077222222770", "788888888887", "778888888877", "077000000770"
  }
};

// Authored facing right: the head is carried forward of the shell rather than
// balanced on top of it, so the sprite flips when walking left.
static const char SHELL_FRAMES[2][SHELL_ROWS][CHAR_COLS + 1] PROGMEM = {
  {
    "000000000000", "000000007700", "000000078870", "000000788787",
    "000770788787", "077557788870", "079a99a78700", "79a99a957000",
    "7a99a99a7000", "799a99a97000", "75a99a957000", "0799a9570000",
    "007777700000", "078877887000", "078877887000", "007700770000"
  },
  {
    "000000000000", "000000007700", "000000078870", "000000788787",
    "000770788787", "077557788870", "079a99a78700", "79a99a957000",
    "7a99a99a7000", "799a99a97000", "75a99a957000", "0799a9570000",
    "007777700000", "788700788700", "788700788700", "077000077000"
  }
};

struct RunnerLayout {
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
  int16_t p;
  int16_t cloudNearY;
  int16_t cloudFarY;
  int16_t nearAmp;
  int16_t farAmp;
  int16_t brickSize;
  int16_t brickY;
  int16_t brickX;
  int16_t brickCount;
  bool parallax;
  bool band;
  bool wideClouds;
};

enum RunnerState : uint8_t {
  RUNNER_IDLE,
  RUNNER_WALKING,
  RUNNER_JUMPING,
  RUNNER_EXITING
};

struct CoinState {
  bool active;
  int16_t x;
  int16_t y;
  int16_t velocity;
  uint8_t frame;
};

RunnerState state = RUNNER_IDLE;
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
int16_t runnerX = 0;
int16_t jumpOffset = 0;
int16_t jumpVelocity = 0;
bool digitHit = false;
bool facingRight = true;
uint8_t walkFrame = 0;
int32_t lastTriggerMinute = -1;
int32_t lastHeaderKey = -1;
int16_t previousRunnerX = -1000;
int16_t previousRunnerY = -1000;
int16_t previousCoinX = -1000;
int16_t previousCoinY = -1000;
bool previousColon = false;
bool timeOverridden = false;
bool initialized = false;
unsigned long lastTick = 0;
unsigned long timeOverrideStart = 0;

class RunnerWrite {
 public:
  RunnerWrite() { tft.startWrite(); }
  ~RunnerWrite() { tft.endWrite(); }
};

int scaled(float value, float unit) {
  return max(1, (int)(value * unit + 0.5f));
}

RunnerLayout currentLayout() {
  RunnerLayout out;
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
  out.p = (int16_t)scaled(2.0f, out.unit);

  // Both extras are gated on measured space rather than on panel identity, so
  // a panel that cannot fit them renders exactly as it does today.
  out.cloudNearY = (int16_t)scaled(18.0f, out.unit);
  out.cloudFarY = (int16_t)scaled(9.0f, out.unit);
  out.nearAmp = (int16_t)scaled(16.0f, out.unit);
  out.farAmp = (int16_t)scaled(10.0f, out.unit);
  out.parallax = out.dateY >= out.cloudFarY + CLOUD_FAR_ROWS * out.p +
                              scaled(4.0f, out.unit);

  // A drifting cloud that reaches into the padded digit cell would force that
  // cell to be cleared and redrawn on every step, which flickers on a panel
  // that writes straight to the glass. Where the tall cloud does not clear the
  // digits, the near layer falls back to the small sprite.
  const int digitCellTop = out.timeY - scaled(14.0f, out.unit);
  out.wideClouds =
    out.cloudNearY + CLOUD_NEAR_ROWS * out.p + 2 <= digitCellTop;

  const int cloudBottom = out.cloudNearY +
    (out.wideClouds ? CLOUD_NEAR_ROWS : CLOUD_FAR_ROWS) * out.p;
  out.brickSize = (int16_t)(8 * out.p);
  out.band = (out.dateY - cloudBottom) >= scaled(46.0f, out.unit);
  out.brickY = (int16_t)(out.dateY - scaled(30.0f, out.unit) - out.brickSize);
  const int edge = scaled(8.0f, out.unit);
  out.brickCount = (int16_t)((out.w - 2 * edge) / out.brickSize);
  out.brickX = (int16_t)((out.w - out.brickCount * out.brickSize) / 2);
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

int digitX(const RunnerLayout& layout, int index) {
  if (index < 2) return layout.timeX + index * layout.digitW;
  if (index == 2) return layout.timeX + 2 * layout.digitW;
  return layout.timeX + 2 * layout.digitW + layout.colonW +
         (index - 3) * layout.digitW;
}

uint16_t spriteColor(char index) {
  switch (index) {
    case '1': return RUNNER_RED;
    case '2': return RUNNER_BROWN;
    case '3': return RUNNER_SKIN;
    case '4': return RUNNER_BLUE;
    case '5': return RUNNER_WHITE;
    case '6': return RUNNER_GOLD;
    case '7': return SCENE_OUTLINE;
    case '8': return CHAR_TAN;
    case '9': return SHELL_GREEN;
    case 'a': return SHELL_DARK;
    default: return SCENE_SKY;
  }
}

// Run-length blitter for the band characters. Unlike drawRunnerSpriteOn these
// sprites carry their own outline in the pixel data, so no halo pass is needed.
void drawIndexedSprite(lgfx::LGFXBase& gfx, const char* data, int cols,
                       int rows, int x, int y, int scale, bool faceRight) {
  for (int row = 0; row < rows; row++) {
    const char* line = data + row * (cols + 1);
    int col = 0;
    while (col < cols) {
      const int sourceCol = faceRight ? col : cols - 1 - col;
      const char color = (char)pgm_read_byte(&line[sourceCol]);
      if (color == '0') {
        col++;
        continue;
      }
      int run = 1;
      while (col + run < cols) {
        const int nextSource = faceRight ? col + run : cols - 1 - col - run;
        if ((char)pgm_read_byte(&line[nextSource]) != color) break;
        run++;
      }
      gfx.fillRect(x + col * scale, y + row * scale, run * scale, scale,
                   spriteColor(color));
      col += run;
    }
  }
}

void drawRunnerSpriteOn(lgfx::LGFXBase& gfx,
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

void drawRunnerSprite(const char frame[SPRITE_ROWS][SPRITE_COLS + 1],
                     int x, int y, int scale, bool faceRight) {
  drawRunnerSpriteOn(tft, frame, x, y, scale, faceRight);
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

void drawPixelCloud(lgfx::LGFXBase& gfx, const char* data, int cols, int rows,
                    int x, int y, int p) {
  for (int row = 0; row < rows; row++) {
    const char* line = data + row * (cols + 1);
    int col = 0;
    while (col < cols) {
      const char color = (char)pgm_read_byte(&line[col]);
      if (color == '0') {
        col++;
        continue;
      }
      int run = 1;
      while (col + run < cols &&
             (char)pgm_read_byte(&line[col + run]) == color)
        run++;
      const uint16_t rgb = color == '1' ? SCENE_OUTLINE :
                           color == '2' ? SCENE_CLOUD : SCENE_CLOUD_SKY;
      gfx.fillRect(x + col * p, y + row * p, run * p, p, rgb);
      col += run;
    }
  }
}

// Each cloud drifts around a home position by a fixed amplitude, so the
// repaint window stays bounded and no cloud ever leaves the panel.
struct CloudState {
  int16_t x;
  int16_t y;
  int16_t minX;
  int16_t maxX;
  int8_t dir;
  uint8_t every;
  uint8_t acc;
  bool wide;
};

struct BandChar {
  int16_t x;
  int16_t minX;
  int16_t maxX;
  int8_t dir;
  uint8_t frameAcc;
  uint8_t frame;
  bool shell;
};

constexpr int MAX_CLOUDS = 5;
constexpr int MAX_BAND_CHARS = 2;
// The Guition flushes a full PSRAM frame whenever anything is marked dirty, so
// scenery runs on its own slower sub-tick instead of every 35 ms.
constexpr int SCENERY_TICKS = 3;

CloudState clouds[MAX_CLOUDS];
uint8_t cloudCount = 0;
BandChar bandChars[MAX_BAND_CHARS];
uint8_t bandCharCount = 0;
uint8_t sceneryAcc = 0;

int cloudWidth(const RunnerLayout& layout, bool wide) {
  return (wide ? CLOUD_NEAR_COLS : CLOUD_FAR_COLS) * layout.p;
}

int cloudHeight(const RunnerLayout& layout, bool wide) {
  return (wide ? CLOUD_NEAR_ROWS : CLOUD_FAR_ROWS) * layout.p;
}

int bandCharHeight(const RunnerLayout& layout, bool shell) {
  return (shell ? SHELL_ROWS : WALKER_ROWS) * layout.spriteScale;
}

void addCloud(const RunnerLayout& layout, bool wide, int home, int y, int amp,
              int8_t dir, uint8_t every, uint8_t phase) {
  if (cloudCount >= MAX_CLOUDS) return;
  CloudState& c = clouds[cloudCount++];
  const int cw = cloudWidth(layout, wide);
  c.wide = wide;
  c.x = (int16_t)constrain(home, 0, max(0, layout.w - cw));
  c.y = (int16_t)y;
  c.minX = (int16_t)max(0, home - amp);
  c.maxX = (int16_t)min(layout.w - cw, home + amp);
  c.dir = dir;
  c.every = every;
  c.acc = phase;
}

void initScenery(const RunnerLayout& layout) {
  cloudCount = 0;
  bandCharCount = 0;
  sceneryAcc = 0;

  const int nearW = cloudWidth(layout, layout.wideClouds);
  const int farW = cloudWidth(layout, false);
  if (layout.parallax) {
    addCloud(layout, false, scaled(24.0f, layout.unit), layout.cloudFarY,
             layout.farAmp, 1, 5, 0);
    addCloud(layout, false, (layout.w - farW) / 2, layout.cloudFarY,
             layout.farAmp, -1, 5, 2);
    addCloud(layout, false, layout.w - farW - scaled(20.0f, layout.unit),
             layout.cloudFarY, layout.farAmp, -1, 5, 4);
  }
  addCloud(layout, layout.wideClouds, scaled(6.0f, layout.unit),
           layout.cloudNearY, layout.nearAmp, 1, 2, 0);
  addCloud(layout, layout.wideClouds, layout.w - nearW - scaled(6.0f, layout.unit),
           layout.cloudNearY, layout.nearAmp, -1, 2, 1);

  if (!layout.band) return;
  const int left = layout.brickX;
  const int right = layout.brickX + layout.brickCount * layout.brickSize;
  const int mid = (left + right) / 2;
  const int charW = CHAR_COLS * layout.spriteScale;
  // Separate patrol halves keep the two characters from colliding without any
  // interaction logic.
  BandChar& walker = bandChars[bandCharCount++];
  walker.shell = false;
  walker.minX = (int16_t)(left + 2);
  walker.maxX = (int16_t)max(left + 2, mid - charW - 2);
  walker.x = (int16_t)(left + layout.brickSize);
  walker.dir = 1;
  walker.frame = 0;
  walker.frameAcc = 0;

  BandChar& shellWalker = bandChars[bandCharCount++];
  shellWalker.shell = true;
  shellWalker.minX = (int16_t)(mid + 2);
  shellWalker.maxX = (int16_t)max(mid + 2, right - charW - 2);
  shellWalker.x = (int16_t)(right - layout.brickSize - charW);
  shellWalker.dir = -1;
  shellWalker.frame = 0;
  shellWalker.frameAcc = 0;
}

void drawCloudsOn(lgfx::LGFXBase& gfx, const RunnerLayout& layout,
                  int originX, int originY) {
  for (uint8_t i = 0; i < cloudCount; i++) {
    const CloudState& c = clouds[i];
    drawPixelCloud(gfx,
                   c.wide ? &CLOUD_NEAR[0][0] : &CLOUD_FAR[0][0],
                   c.wide ? CLOUD_NEAR_COLS : CLOUD_FAR_COLS,
                   c.wide ? CLOUD_NEAR_ROWS : CLOUD_FAR_ROWS,
                   c.x - originX, c.y - originY, layout.p);
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
  gfx.fillRect(x + p, y + p, p, p, RUNNER_WHITE);
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

// Brick platform plus the two patrolling characters, drawn into the sky gap a
// tall panel leaves between the clouds and the date.
void drawSkyBandOn(lgfx::LGFXBase& gfx, const RunnerLayout& layout,
                   int originX, int originY) {
  if (!layout.band) return;
  const int y = layout.brickY - originY;
  for (int i = 0; i < layout.brickCount; i++) {
    const int x = layout.brickX + i * layout.brickSize - originX;
    if (i % 4 == 2) drawQuestionBlock(gfx, x, y, layout.p);
    else drawBrickBlock(gfx, x, y, layout.p);
  }
  for (uint8_t i = 0; i < bandCharCount; i++) {
    const BandChar& c = bandChars[i];
    const int h = bandCharHeight(layout, c.shell);
    const char* data = c.shell
      ? &SHELL_FRAMES[c.frame & 1][0][0]
      : &WALKER_FRAMES[c.frame & 1][0][0];
    // The shell walker is authored facing right, so it flips when walking left.
    drawIndexedSprite(gfx, data, CHAR_COLS, c.shell ? SHELL_ROWS : WALKER_ROWS,
                      c.x - originX, layout.brickY - h - originY,
                      layout.spriteScale, !c.shell || c.dir >= 0);
  }
}

void drawBackgroundSceneryOn(lgfx::LGFXBase& gfx, const RunnerLayout& layout,
                             int originX, int originY) {
  const int p = scaled(2.0f, layout.unit);
  const int blockSize = 8 * p;
  const int groupW = 5 * blockSize;
  const int blockX = layout.w - groupW - scaled(12.0f, layout.unit) - originX;
  const int blockY = layout.groundY - scaled(50.0f, layout.unit) - originY;
  const int groundY = layout.groundY - originY;

  drawCloudsOn(gfx, layout, originX, originY);
  drawSkyBandOn(gfx, layout, originX, originY);
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

void drawBackgroundScenery(const RunnerLayout& layout) {
  drawBackgroundSceneryOn(tft, layout, 0, 0);
}

void drawGroundOn(lgfx::LGFXBase& gfx, const RunnerLayout& layout,
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

void drawGround(const RunnerLayout& layout) {
  drawGroundOn(tft, layout, 0, 0);
}

void drawHeader(const RunnerLayout& layout, const struct tm& now, bool clear) {
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

void drawDigits(const RunnerLayout& layout, const char digits[5], bool colonOn,
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

void drawDirtyDigits(const RunnerLayout& layout, const char digits[5],
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

void drawColon(const RunnerLayout& layout, bool colonOn) {
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

void restoreBackgroundRect(const RunnerLayout& layout,
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

uint8_t digitMaskForRect(const RunnerLayout& layout, int x, int y, int w, int h) {
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

struct DirtyRect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

constexpr int MAX_SCENERY_RECTS = MAX_CLOUDS + MAX_BAND_CHARS;

void ensureScenery(const RunnerLayout& layout) {
  if (cloudCount == 0) initScenery(layout);
}

// Advances clouds and band characters, collecting the rectangles that changed.
// A cloud only steps once every few scenery ticks, so most calls return few
// rects and a fully idle tick returns none at all.
uint8_t advanceScenery(const RunnerLayout& layout, DirtyRect* out) {
  ensureScenery(layout);
  if (++sceneryAcc < SCENERY_TICKS) return 0;
  sceneryAcc = 0;

  uint8_t count = 0;
  for (uint8_t i = 0; i < cloudCount; i++) {
    CloudState& c = clouds[i];
    if (c.maxX <= c.minX) continue;
    if (++c.acc < c.every) continue;
    c.acc = 0;
    int next = c.x + c.dir;
    if (next <= c.minX) {
      next = c.minX;
      c.dir = 1;
    } else if (next >= c.maxX) {
      next = c.maxX;
      c.dir = -1;
    }
    if (next == c.x) continue;
    out[count].x = (int16_t)(min((int)c.x, next) - 1);
    out[count].y = (int16_t)(c.y - 1);
    out[count].w = (int16_t)(cloudWidth(layout, c.wide) + 3);
    out[count].h = (int16_t)(cloudHeight(layout, c.wide) + 2);
    count++;
    c.x = (int16_t)next;
  }

  const int step = max(1, layout.spriteScale * 2 / 3);
  for (uint8_t i = 0; i < bandCharCount; i++) {
    BandChar& c = bandChars[i];
    if (c.maxX <= c.minX) continue;
    int next = c.x + c.dir * step;
    if (next <= c.minX) {
      next = c.minX;
      c.dir = 1;
    } else if (next >= c.maxX) {
      next = c.maxX;
      c.dir = -1;
    }
    if (++c.frameAcc >= 3) {
      c.frameAcc = 0;
      c.frame ^= 1;
    }
    const int height = bandCharHeight(layout, c.shell);
    out[count].x = (int16_t)(min((int)c.x, next) - 1);
    out[count].y = (int16_t)(layout.brickY - height - 1);
    out[count].w = (int16_t)(CHAR_COLS * layout.spriteScale +
                             abs(next - c.x) + 2);
    out[count].h = (int16_t)(height + 2);
    count++;
    c.x = (int16_t)next;
  }
  return count;
}

// Every restore repaints the whole scene clipped to one rect, so the cost is
// per rect rather than per pixel. Clouds share a horizontal strip and the band
// characters share another, so merging rects that overlap vertically collapses
// a typical tick from up to seven restores down to two.
uint8_t mergeSceneryRects(DirtyRect* rects, uint8_t count) {
  bool merged = true;
  while (merged) {
    merged = false;
    for (uint8_t i = 0; i < count && !merged; i++) {
      for (uint8_t j = (uint8_t)(i + 1); j < count && !merged; j++) {
        const int aTop = rects[i].y;
        const int aBottom = rects[i].y + rects[i].h;
        const int bTop = rects[j].y;
        const int bBottom = rects[j].y + rects[j].h;
        if (aTop >= bBottom || bTop >= aBottom) continue;
        const int left = min(rects[i].x, rects[j].x);
        const int top = min(aTop, bTop);
        const int right = max(rects[i].x + rects[i].w, rects[j].x + rects[j].w);
        const int bottom = max(aBottom, bBottom);
        rects[i].x = (int16_t)left;
        rects[i].y = (int16_t)top;
        rects[i].w = (int16_t)(right - left);
        rects[i].h = (int16_t)(bottom - top);
        rects[j] = rects[count - 1];
        count--;
        merged = true;
      }
    }
  }
  return count;
}

int coinWide(const RunnerLayout& layout) {
  return scaled(12.0f, layout.unit);
}

int coinHeight(const RunnerLayout& layout) {
  return scaled(16.0f, layout.unit);
}

uint8_t erasePreviousDynamic(const RunnerLayout& layout) {
  uint8_t restoreMask = 0;
  if (previousRunnerX > -900) {
    restoreMask |= digitMaskForRect(
      layout, previousRunnerX - 1, previousRunnerY - 1,
      layout.spriteW + 2, layout.spriteH + 2);
    restoreBackgroundRect(layout, previousRunnerX - 1, previousRunnerY - 1,
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
  previousRunnerX = previousRunnerY = -1000;
  previousCoinX = previousCoinY = -1000;
  return restoreMask;
}

uint8_t erasePreviousCoin(const RunnerLayout& layout) {
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

void paintCoinOn(lgfx::LGFXBase& gfx, const RunnerLayout& layout,
                 int originX, int originY) {
  if (!coin.active) return;
  const int wide = coinWide(layout);
  const int narrow = scaled(4.0f, layout.unit);
  const int height = coinHeight(layout);
  const int width = ((coin.frame / 3) & 1) ? narrow : wide;
  const int x = coin.x + (wide - width) / 2 - originX;
  const int y = coin.y - originY;
  gfx.fillRoundRect(x, y, width, height, max(1, width / 2), RUNNER_GOLD);
  if (width > narrow)
    gfx.drawFastVLine(x + width / 2, y + 2,
                      max(1, height - 4), RUNNER_WHITE);
}

void drawCoin(const RunnerLayout& layout) {
  if (!coin.active) return;
  paintCoinOn(tft, layout, 0, 0);
  previousCoinX = coin.x;
  previousCoinY = coin.y;
}

void spawnCoin(const RunnerLayout& layout, int index) {
  coin.active = true;
  coin.x = digitX(layout, index) + layout.digitW / 2 - coinWide(layout) / 2;
  coin.y = layout.timeY + scaled(4.0f, layout.unit);
  coin.velocity = -scaled(8.0f, layout.unit);
  coin.frame = 0;
}

void updateCoin(const RunnerLayout& layout) {
  if (!coin.active) return;
  coin.y += coin.velocity;
  coin.velocity += max(1, layout.spriteScale / 2);
  coin.frame++;
  if (coin.frame > 20 || coin.y > layout.timeY + scaled(4.0f, layout.unit))
    coin.active = false;
}

void updateDigitBounce(const RunnerLayout& layout) {
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

void beginMinuteChange(const RunnerLayout& layout, const struct tm& next) {
  buildDigits(next, targetDigits);
  targetCount = 0;
  static const uint8_t digitIndices[MAX_TARGETS] = {0, 1, 3, 4};
  for (uint8_t index : digitIndices) {
    if (displayedDigits[index] != targetDigits[index])
      targets[targetCount++] = index;
  }
  if (targetCount == 0) return;
  targetIndex = 0;
  runnerX = -layout.spriteW;
  jumpOffset = 0;
  jumpVelocity = 0;
  digitHit = false;
  facingRight = true;
  walkFrame = 0;
  state = RUNNER_WALKING;
}

int targetRunnerX(const RunnerLayout& layout) {
  const int index = targets[targetIndex];
  return digitX(layout, index) + layout.digitW / 2 - layout.spriteW / 2;
}

void advanceAnimation(const RunnerLayout& layout) {
  updateDigitBounce(layout);
  updateCoin(layout);
  const int walkStep = max(3, layout.spriteScale * 5 / 2);
  const int gravity = max(1, layout.spriteScale / 2);

  switch (state) {
    case RUNNER_WALKING: {
      const int target = targetRunnerX(layout);
      const int delta = target - runnerX;
      if (abs(delta) <= walkStep) {
        runnerX = target;
        state = RUNNER_JUMPING;
        jumpOffset = 0;
        jumpVelocity = -max(8, layout.spriteScale * 9 / 2);
        digitHit = false;
      } else {
        facingRight = delta > 0;
        runnerX += facingRight ? walkStep : -walkStep;
        walkFrame = (walkFrame + 1) % 8;
      }
      break;
    }
    case RUNNER_JUMPING: {
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
          facingRight = targetRunnerX(layout) >= runnerX;
          state = RUNNER_WALKING;
        } else {
          facingRight = true;
          state = RUNNER_EXITING;
        }
      }
      break;
    }
    case RUNNER_EXITING:
      runnerX += walkStep;
      walkFrame = (walkFrame + 1) % 8;
      if (runnerX > layout.w + layout.spriteW) state = RUNNER_IDLE;
      break;
    default:
      break;
  }
}

const char (*activeRunnerFrame())[SPRITE_COLS + 1] {
  const char (*frame)[SPRITE_COLS + 1] = RUNNER_STAND;
  if (state == RUNNER_JUMPING) frame = RUNNER_JUMP;
  else {
    switch ((walkFrame / 2) & 3) {
      case 0: frame = RUNNER_WALK_A; break;
      case 1: frame = RUNNER_WALK_B; break;
      case 2: frame = RUNNER_WALK_C; break;
      default: frame = RUNNER_WALK_D; break;
    }
  }
  return frame;
}

void paintDigitsOn(lgfx::LGFXBase& gfx, const RunnerLayout& layout,
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

bool composeRunnerTransition(const RunnerLayout& layout,
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
  if (right - left > RUNNER_DIRTY_SIZE || bottom - top > RUNNER_DIRTY_SIZE)
    return false;
  // The full 64x64 sprite is pushed each time, so keep the window entirely
  // on-panel. A partially clipped push smears columns at the right edge
  // during the runner's exit walk (seen with LovyanGFX 1.2.25 on the C3).
  if (layout.w >= RUNNER_DIRTY_SIZE)
    left = min(left, (int)(layout.w - RUNNER_DIRTY_SIZE));
  if (layout.h >= RUNNER_DIRTY_SIZE)
    top = min(top, (int)(layout.h - RUNNER_DIRTY_SIZE));

  static lgfx::LGFX_Sprite dirtySprite;
  static bool spriteReady = false;
  if (!spriteReady) {
    dirtySprite.setColorDepth(16);
    spriteReady = dirtySprite.createSprite(RUNNER_DIRTY_SIZE, RUNNER_DIRTY_SIZE);
    if (!spriteReady) return false;
  }

  dirtySprite.fillSprite(SCENE_SKY);
  drawBackgroundSceneryOn(dirtySprite, layout, left, top);
  drawGroundOn(dirtySprite, layout, left, top);
  paintDigitsOn(dirtySprite, layout, left, top, colonOn);
  paintCoinOn(dirtySprite, layout, left, top);
  if (newVisible)
    drawRunnerSpriteOn(dirtySprite, newFrame, newX - left, newY - top,
                      layout.spriteScale, facingRight);
  dirtySprite.pushSprite(tft_ptr, left, top);
  tft.waitDMA();
  return true;
}

void drawAnimatedRunner(const RunnerLayout& layout) {
  if (state == RUNNER_IDLE) return;
  const int y = layout.groundY - layout.spriteH + jumpOffset;
  const char (*frame)[SPRITE_COLS + 1] = activeRunnerFrame();
  drawRunnerSprite(frame, runnerX, y, layout.spriteScale, facingRight);
  previousRunnerX = runnerX;
  previousRunnerY = y;
}

int32_t headerKey(const struct tm& now) {
  const int pm = !netSettings.use24h && now.tm_hour >= 12 ? 1 : 0;
  return ((now.tm_year * 367 + now.tm_yday) * 2) + pm;
}

void initialize(const RunnerLayout& layout, const struct tm& now) {
  tft.fillScreen(SCENE_SKY);
  initScenery(layout);
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
  state = RUNNER_IDLE;
  coin.active = false;
  timeOverridden = false;
  previousRunnerX = previousRunnerY = -1000;
  previousCoinX = previousCoinY = -1000;
  initialized = true;
}

void drawSnapshotScene(const RunnerLayout& layout, const struct tm& now) {
  char digits[5];
  int16_t offsets[5] = {0};
  buildDigits(now, digits);
  tft.fillScreen(SCENE_SKY);
  ensureScenery(layout);
  drawBackgroundScenery(layout);
  drawGround(layout);
  drawHeader(layout, now, false);
  drawDigits(layout, digits, (millis() % 1000) < 500, offsets, false);
  const int x = scaled(14.0f, layout.unit);
  const int y = layout.groundY - layout.spriteH;
  drawRunnerSprite(RUNNER_STAND, x, y, layout.spriteScale, true);
}

} // namespace

void resetRunnerClock() {
  initialized = false;
  lastTick = 0;
  cloudCount = 0;
  bandCharCount = 0;
  sceneryAcc = 0;
  state = RUNNER_IDLE;
  coin.active = false;
  timeOverridden = false;
  timeOverrideStart = 0;
  lastTriggerMinute = -1;
  previousRunnerX = previousRunnerY = -1000;
  previousCoinX = previousCoinY = -1000;
}

void tickRunnerClock() {
  const unsigned long nowMs = millis();
  if (initialized && nowMs - lastTick < UPDATE_MS) return;
  lastTick = nowMs;

  struct tm now;
  time_t raw;
  if (!readTime(now, raw)) return;
  const RunnerLayout layout = currentLayout();
  RunnerWrite write;

  if (!initialized) {
    initialize(layout, now);
    markFrameDirty();
    return;
  }

  const bool colonOn = (nowMs % 1000) < 500;
  char currentDigits[5];
  buildDigits(now, currentDigits);
  bool changed = false;

  // Scenery runs whether or not the runner is on screen. It sits well above the
  // digits on every panel that enables the band, but the 240 x 240 near clouds
  // reach the top of the digit cells, so restored rects still feed the digit
  // mask.
  DirtyRect sceneryRects[MAX_SCENERY_RECTS];
  uint8_t sceneryCount = advanceScenery(layout, sceneryRects);
  sceneryCount = mergeSceneryRects(sceneryRects, sceneryCount);
  if (sceneryCount > 0) {
    uint8_t sceneryMask = 0;
    for (uint8_t i = 0; i < sceneryCount; i++) {
      const DirtyRect& r = sceneryRects[i];
      sceneryMask |= digitMaskForRect(layout, r.x, r.y, r.w, r.h);
      restoreBackgroundRect(layout, r.x, r.y, r.w, r.h);
    }
    if (sceneryMask)
      drawDirtyDigits(layout, displayedDigits, colonOn, digitOffset,
                      sceneryMask);
    changed = true;
  }
  if (headerKey(now) != lastHeaderKey) {
    drawHeader(layout, now, true);
    lastHeaderKey = headerKey(now);
    changed = true;
  }

  if (timeOverridden) {
    const bool timeCaughtUp =
      state == RUNNER_IDLE &&
      memcmp(displayedDigits, currentDigits, sizeof(displayedDigits)) == 0;
    const bool timedOut = nowMs - timeOverrideStart > TIME_OVERRIDE_MAX_MS;
    if (timeCaughtUp) {
      timeOverridden = false;
    } else if (timedOut) {
      const uint8_t restoreMask = erasePreviousDynamic(layout);
      state = RUNNER_IDLE;
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

  if (state == RUNNER_IDLE) {
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

  if (state != RUNNER_IDLE || coin.active) {
    const int oldRunnerX = previousRunnerX;
    const int oldRunnerY = previousRunnerY;
    const uint8_t restoreMask = erasePreviousCoin(layout);
    advanceAnimation(layout);
    drawDirtyDigits(layout, displayedDigits, colonOn, digitOffset, restoreMask);
    drawCoin(layout);
    const bool newRunnerVisible = state != RUNNER_IDLE;
    const int newRunnerY = layout.groundY - layout.spriteH + jumpOffset;
    const char (*newFrame)[SPRITE_COLS + 1] = activeRunnerFrame();
    if (composeRunnerTransition(layout, oldRunnerX, oldRunnerY,
                               newRunnerVisible, runnerX, newRunnerY,
                               newFrame, colonOn)) {
      if (newRunnerVisible) {
        previousRunnerX = runnerX;
        previousRunnerY = newRunnerY;
      } else {
        previousRunnerX = previousRunnerY = -1000;
      }
    } else {
      uint8_t runnerMask = 0;
      if (oldRunnerX > -900) {
        runnerMask = digitMaskForRect(
          layout, oldRunnerX - 1, oldRunnerY - 1,
          layout.spriteW + 2, layout.spriteH + 2);
        restoreBackgroundRect(layout, oldRunnerX - 1, oldRunnerY - 1,
                              layout.spriteW + 2, layout.spriteH + 2);
      }
      drawDirtyDigits(layout, displayedDigits, colonOn, digitOffset, runnerMask);
      drawCoin(layout);
      drawAnimatedRunner(layout);
      if (!newRunnerVisible)
        previousRunnerX = previousRunnerY = -1000;
    }
    changed = true;
  } else if (colonOn != previousColon) {
    drawColon(layout, colonOn);
    previousColon = colonOn;
    changed = true;
  }

  if (changed) markFrameDirty();
}

void drawRunnerClockSnapshot() {
  struct tm now;
  time_t raw;
  if (!readTime(now, raw)) return;
  const RunnerLayout layout = currentLayout();
  RunnerWrite write;
  drawSnapshotScene(layout, now);
  markFrameDirty();
}
