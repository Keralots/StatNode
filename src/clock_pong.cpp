#include "clock_pong.h"
#include "config.h"
#include "display_ui.h"
#include "fonts.h"
#include "layout.h"
#include "settings.h"
#include <math.h>
#include <time.h>

namespace {

constexpr int BRICK_ROWS = LY_ARK_BRICK_ROWS;
constexpr int MAX_BRICK_COLS = 13;
constexpr int UPDATE_MS = 20;
constexpr float BALL_SPEED = 3.0f;
constexpr int PADDLE_SPEED = 4;
constexpr uint16_t brickColors[5] = {0xF800, 0xFD20, 0xFFE0, 0x07E0, 0x001F};

static_assert(BRICK_ROWS <= 5, "Breakout layout has more rows than colors");
static_assert(LY_ARK_COLS <= MAX_BRICK_COLS, "Breakout layout exceeds brick storage");

struct PongLayout {
  int16_t w;
  int16_t h;
  int16_t cols;
  int16_t brickX;
  int16_t brickY;
  int16_t paddleY;
  int16_t paddleW;
  int16_t timeY;
  int16_t dateY;
  int16_t digitW;
  int16_t digitH;
  int16_t colonW;
};

bool bricks[BRICK_ROWS][MAX_BRICK_COLS];
int brickCount = 0;
float ballX = 0;
float ballY = 0;
float ballVX = 0;
float ballVY = 0;
int prevBallX = -1;
int prevBallY = -1;
int paddleX = 0;
int prevPaddleX = -1;
int displayedHour = -1;
int displayedMinute = -1;
float digitOffset[5] = {0};
float digitVelocity[5] = {0};
int prevDigitY[5] = {-1, -1, -1, -1, -1};
char prevDigits[5] = {0};
bool prevColon = false;
char prevDate[28] = {0};
bool initialized = false;
unsigned long lastTick = 0;

class PongWrite {
 public:
  PongWrite() { tft.startWrite(); }
  ~PongWrite() { tft.endWrite(); }
};

PongLayout currentLayout() {
  PongLayout out = {
    (int16_t)tft.width(), (int16_t)tft.height(), LY_ARK_COLS,
    LY_ARK_START_X, LY_ARK_START_Y, LY_ARK_PADDLE_Y, LY_ARK_PADDLE_W,
    LY_ARK_TIME_Y, LY_ARK_DATE_Y, LY_ARK_DIGIT_W, LY_ARK_DIGIT_H,
    LY_ARK_COLON_W
  };
#if defined(LAYOUT_HAS_LANDSCAPE)
  if (out.w > out.h) {
    out.cols = LY_LAND_ARK_BRICK_COLS;
    out.brickX = (out.w - (out.cols * LY_ARK_BRICK_W +
                  (out.cols - 1) * LY_ARK_BRICK_GAP)) / 2;
    out.brickY = LY_LAND_ARK_BRICK_START_Y;
    out.paddleY = LY_LAND_ARK_PADDLE_Y;
    out.paddleW = LY_LAND_ARK_PADDLE_W;
    out.timeY = LY_LAND_ARK_TIME_Y;
    out.dateY = LY_LAND_ARK_DATE_Y;
  }
#endif
  return out;
}

int ballSize(const PongLayout& layout) { return layout.w >= 480 ? 8 : 4; }
int paddleHeight(const PongLayout& layout) { return layout.w >= 480 ? 8 : 4; }
float textScale(const PongLayout& layout) { return layout.w >= 480 ? 2.0f : 1.0f; }

bool readTime(struct tm& now) {
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

int timeStartX(const PongLayout& layout) {
  return (layout.w - (4 * layout.digitW + layout.colonW)) / 2;
}

int digitX(const PongLayout& layout, int index) {
  const int start = timeStartX(layout);
  if (index < 2) return start + index * layout.digitW;
  if (index == 2) return start + 2 * layout.digitW;
  return start + 2 * layout.digitW + layout.colonW + (index - 3) * layout.digitW;
}

void buildDigits(int hour, int minute, char out[5]) {
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
  out[3] = '0' + minute / 10;
  out[4] = '0' + minute % 10;
}

void initBricks(const PongLayout& layout) {
  brickCount = 0;
  for (int row = 0; row < BRICK_ROWS; row++) {
    for (int col = 0; col < MAX_BRICK_COLS; col++) {
      bricks[row][col] = col < layout.cols;
      if (bricks[row][col]) brickCount++;
    }
  }
}

void drawBrick(const PongLayout& layout, int row, int col) {
  const int x = layout.brickX + col * (LY_ARK_BRICK_W + LY_ARK_BRICK_GAP);
  const int y = layout.brickY + row * (LY_ARK_BRICK_H + LY_ARK_BRICK_GAP);
  tft.fillRect(x, y, LY_ARK_BRICK_W, LY_ARK_BRICK_H, brickColors[row]);
  tft.drawFastHLine(x, y, LY_ARK_BRICK_W, TFT_WHITE);
  tft.drawFastHLine(x, y + LY_ARK_BRICK_H - 1, LY_ARK_BRICK_W, dispSettings.bgColor);
}

void drawAllBricks(const PongLayout& layout, bool activeOnly = true) {
  for (int row = 0; row < BRICK_ROWS; row++)
    for (int col = 0; col < layout.cols; col++)
      if (!activeOnly || bricks[row][col]) drawBrick(layout, row, col);
}

void clearBrick(const PongLayout& layout, int row, int col) {
  const int x = layout.brickX + col * (LY_ARK_BRICK_W + LY_ARK_BRICK_GAP);
  const int y = layout.brickY + row * (LY_ARK_BRICK_H + LY_ARK_BRICK_GAP);
  tft.fillRect(max(0, x - LY_ARK_BRICK_GAP), max(0, y - LY_ARK_BRICK_GAP),
               LY_ARK_BRICK_W + 2 * LY_ARK_BRICK_GAP,
               LY_ARK_BRICK_H + 2 * LY_ARK_BRICK_GAP, dispSettings.bgColor);
}

void launchBall(const PongLayout& layout) {
  const int size = ballSize(layout);
  ballX = paddleX - size / 2.0f;
  ballY = layout.paddleY - size - 1;
  float angle = random(35, 146) * PI / 180.0f;
  ballVX = BALL_SPEED * cosf(angle);
  ballVY = -fabsf(BALL_SPEED * sinf(angle));
  if (fabsf(ballVX) < 1.2f) ballVX = ballVX < 0 ? -1.2f : 1.2f;
}

bool overlapsClockText(const PongLayout& layout, int x, int y) {
  const int size = ballSize(layout);
  const int left = timeStartX(layout) - 2;
  const int right = left + 4 * layout.digitW + layout.colonW + 4;
  if (x + size > left && x < right &&
      y + size > layout.timeY - 12 && y < layout.timeY + layout.digitH + 2)
    return true;
  if (!dispSettings.hideClockDate && y + size > layout.dateY - 2 && y < layout.dateY + 18)
    return true;
  return false;
}

void drawBall(const PongLayout& layout) {
  const int size = ballSize(layout);
  if (prevBallX >= 0 && !overlapsClockText(layout, prevBallX, prevBallY))
    tft.fillRect(prevBallX, prevBallY, size, size, dispSettings.bgColor);
  const int x = (int)ballX;
  const int y = (int)ballY;
  if (!overlapsClockText(layout, x, y))
    tft.fillRect(x, y, size, size, TFT_WHITE);
  prevBallX = x;
  prevBallY = y;
}

void updatePaddle(const PongLayout& layout) {
  const int target = ballVY > 0 ? (int)ballX + ballSize(layout) / 2 : layout.w / 2;
  const int delta = target - paddleX;
  if (abs(delta) > 2) paddleX += delta > 0 ? min(PADDLE_SPEED, delta) : max(-PADDLE_SPEED, delta);
  paddleX = constrain(paddleX, layout.paddleW / 2, layout.w - layout.paddleW / 2);
}

void drawPaddle(const PongLayout& layout) {
  const int height = paddleHeight(layout);
  if (prevPaddleX >= 0 && prevPaddleX != paddleX)
    tft.fillRect(prevPaddleX - layout.paddleW / 2, layout.paddleY,
                 layout.paddleW, height, dispSettings.bgColor);
  tft.fillRect(paddleX - layout.paddleW / 2, layout.paddleY,
               layout.paddleW, height, TFT_CYAN);
  tft.drawFastHLine(paddleX - layout.paddleW / 2, layout.paddleY,
                    layout.paddleW, TFT_WHITE);
  prevPaddleX = paddleX;
}

void checkBrickCollision(const PongLayout& layout) {
  const int size = ballSize(layout);
  for (int row = 0; row < BRICK_ROWS; row++) {
    for (int col = 0; col < layout.cols; col++) {
      if (!bricks[row][col]) continue;
      const int x = layout.brickX + col * (LY_ARK_BRICK_W + LY_ARK_BRICK_GAP);
      const int y = layout.brickY + row * (LY_ARK_BRICK_H + LY_ARK_BRICK_GAP);
      if (ballX + size <= x || ballX >= x + LY_ARK_BRICK_W ||
          ballY + size <= y || ballY >= y + LY_ARK_BRICK_H) continue;
      const float overlapX = min(ballX + size - x, x + LY_ARK_BRICK_W - ballX);
      const float overlapY = min(ballY + size - y, y + LY_ARK_BRICK_H - ballY);
      if (overlapX < overlapY) ballVX = -ballVX; else ballVY = -ballVY;
      bricks[row][col] = false;
      brickCount--;
      clearBrick(layout, row, col);
      ballX += ballVX;
      ballY += ballVY;
      return;
    }
  }
}

void updateBall(const PongLayout& layout) {
  const int size = ballSize(layout);
  ballX += ballVX;
  ballY += ballVY;
  if (ballX <= 0) { ballX = 0; ballVX = fabsf(ballVX); }
  if (ballX >= layout.w - size) { ballX = layout.w - size; ballVX = -fabsf(ballVX); }
  if (ballY <= 0) { ballY = 0; ballVY = fabsf(ballVY); }

  const int paddleLeft = paddleX - layout.paddleW / 2;
  if (ballVY > 0 && ballY + size >= layout.paddleY &&
      ballY < layout.paddleY + paddleHeight(layout) &&
      ballX + size >= paddleLeft && ballX <= paddleLeft + layout.paddleW) {
    float hit = (ballX + size / 2.0f - paddleLeft) / layout.paddleW;
    hit = constrain(hit, 0.08f, 0.92f);
    const float angle = (150.0f - hit * 120.0f + random(-40, 41) / 10.0f) * PI / 180.0f;
    ballVX = BALL_SPEED * cosf(angle);
    ballVY = -fabsf(BALL_SPEED * sinf(angle));
    ballY = layout.paddleY - size - 1;
  }
  if (ballY > layout.h) launchBall(layout);
  checkBrickCollision(layout);
  if (brickCount == 0) {
    initBricks(layout);
    drawAllBricks(layout);
  }
}

void updateDigitBounce(const char nextDigits[5]) {
  for (int i = 0; i < 5; i++) {
    if (i != 2 && prevDigits[i] && prevDigits[i] != nextDigits[i]) {
      digitOffset[i] = -10.0f;
      digitVelocity[i] = 0.8f;
    }
  }
  for (int i = 0; i < 5; i++) {
    if (digitOffset[i] == 0 && digitVelocity[i] == 0) continue;
    digitOffset[i] += digitVelocity[i];
    digitVelocity[i] += 1.0f;
    if (digitOffset[i] >= 0) {
      digitOffset[i] = 0;
      digitVelocity[i] = 0;
    }
  }
}

void drawTime(const PongLayout& layout, int hour, int minute, bool force) {
  char digits[5];
  buildDigits(hour, minute, digits);
  updateDigitBounce(digits);
  setFont(tft, FONT_7SEG);
  tft.setTextSize(textScale(layout));
  tft.setTextColor(dispSettings.clockTimeColor, dispSettings.bgColor);
  tft.setTextDatum(TL_DATUM);

  for (int i = 0; i < 5; i++) {
    if (i == 2) continue;
    const int y = layout.timeY + (int)digitOffset[i];
    const bool moving = digitOffset[i] != 0 || digitVelocity[i] != 0;
    if (!force && !moving && digits[i] == prevDigits[i]) continue;
    if (prevDigitY[i] >= 0)
      tft.fillRect(digitX(layout, i), prevDigitY[i] - 1,
                   layout.digitW + 2, layout.digitH + 12, dispSettings.bgColor);
    tft.drawChar(digits[i], digitX(layout, i), y, 7);
    prevDigitY[i] = y;
    prevDigits[i] = digits[i];
  }

  const bool colon = (millis() % 1000) < 500;
  if (force || colon != prevColon) {
    const int x = digitX(layout, 2);
    tft.fillRect(x, layout.timeY, layout.colonW, layout.digitH, dispSettings.bgColor);
    if (colon) tft.drawChar(':', x, layout.timeY, 7);
    prevColon = colon;
  }
}

void drawDate(const PongLayout& layout, const struct tm& now, bool force) {
  if (dispSettings.hideClockDate) {
    if (force || prevDate[0])
      tft.fillRect(0, layout.dateY, layout.w, 18, dispSettings.bgColor);
    prevDate[0] = 0;
    return;
  }
  char date[28];
  formatDate(now, date, sizeof(date));
  if (!force && strcmp(date, prevDate) == 0) return;
  setFont(tft, FONT_BODY);
  tft.setTextSize(1);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(dispSettings.clockDateColor, dispSettings.bgColor);
  tft.fillRect(0, layout.dateY, layout.w, 18, dispSettings.bgColor);
  tft.drawString(date, layout.w / 2, layout.dateY);
  tft.setTextDatum(TL_DATUM);
  strlcpy(prevDate, date, sizeof(prevDate));
}

void initialize(const PongLayout& layout, const struct tm& now) {
  tft.fillScreen(dispSettings.bgColor);
  initBricks(layout);
  drawAllBricks(layout);
  paddleX = layout.w / 2;
  prevPaddleX = -1;
  prevBallX = -1;
  prevBallY = -1;
  displayedHour = now.tm_hour;
  displayedMinute = now.tm_min;
  launchBall(layout);
  memset(prevDigits, 0, sizeof(prevDigits));
  memset(prevDate, 0, sizeof(prevDate));
  for (int i = 0; i < 5; i++) prevDigitY[i] = -1;
  prevColon = !(millis() % 1000 < 500);
  drawPaddle(layout);
  drawBall(layout);
  drawDate(layout, now, true);
  drawTime(layout, displayedHour, displayedMinute, true);
  initialized = true;
}

void drawSnapshotTime(const PongLayout& layout, const struct tm& now) {
  char digits[5];
  buildDigits(now.tm_hour, now.tm_min, digits);
  setFont(tft, FONT_7SEG);
  tft.setTextSize(textScale(layout));
  tft.setTextColor(dispSettings.clockTimeColor, dispSettings.bgColor);
  tft.setTextDatum(TL_DATUM);
  for (int i = 0; i < 5; i++) {
    if (i == 2 && (millis() % 1000) >= 500) continue;
    tft.drawChar(digits[i], digitX(layout, i), layout.timeY, 7);
  }
  if (!dispSettings.hideClockDate) {
    char date[28];
    formatDate(now, date, sizeof(date));
    setFont(tft, FONT_BODY);
    tft.setTextSize(1);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(dispSettings.clockDateColor, dispSettings.bgColor);
    tft.drawString(date, layout.w / 2, layout.dateY);
  }
  tft.setTextDatum(TL_DATUM);
}

} // namespace

void resetPongClock() {
  initialized = false;
  lastTick = 0;
  prevBallX = -1;
  prevBallY = -1;
  prevPaddleX = -1;
}

void tickPongClock() {
  const unsigned long nowMs = millis();
  if (initialized && nowMs - lastTick < UPDATE_MS) return;
  lastTick = nowMs;
  struct tm now;
  if (!readTime(now)) return;
  const PongLayout layout = currentLayout();
  PongWrite write;
  if (!initialized) {
    initialize(layout, now);
    markFrameDirty();
    return;
  }

  if (now.tm_hour != displayedHour || now.tm_min != displayedMinute) {
    displayedHour = now.tm_hour;
    displayedMinute = now.tm_min;
  }
  updateBall(layout);
  updatePaddle(layout);
  drawBall(layout);
  drawPaddle(layout);
  drawDate(layout, now, false);
  drawTime(layout, displayedHour, displayedMinute, false);
  markFrameDirty();
}

void drawPongClockSnapshot() {
  struct tm now;
  if (!readTime(now)) return;
  const PongLayout layout = currentLayout();
  PongWrite write;
  tft.fillScreen(dispSettings.bgColor);
  for (int row = 0; row < BRICK_ROWS; row++)
    for (int col = 0; col < layout.cols; col++) drawBrick(layout, row, col);
  const int paddle = layout.w / 2;
  tft.fillRect(paddle - layout.paddleW / 2, layout.paddleY,
               layout.paddleW, paddleHeight(layout), TFT_CYAN);
  tft.drawFastHLine(paddle - layout.paddleW / 2, layout.paddleY,
                    layout.paddleW, TFT_WHITE);
  const int size = ballSize(layout);
  tft.fillRect(paddle - size / 2, layout.paddleY - 32, size, size, TFT_WHITE);
  drawSnapshotTime(layout, now);
  markFrameDirty();
}
