#include "touch_button.h"
#include "config.h"
#include "display_ui.h"
#include "settings.h"

static uint8_t configuredPin = 0xFF;
static bool pinReady = false;
static bool rawPressed = false;
static bool stablePressed = false;
static bool consumeUntilRelease = false;
static bool longActionFired = false;
static uint32_t rawChangedMs = 0;
static uint32_t pressedMs = 0;
static uint32_t lastEventMs = 0;
static uint32_t eventCount = 0;
static const char* lastAction = "none";

bool touchInputSupported() {
#if defined(BOARD_IS_C3)
  return true;
#else
  return false;
#endif
}

bool touchPinAllowed(uint8_t pin) {
#if defined(BOARD_IS_C3)
  // GPIO 2/8/9 are strapping pins, 12..17 serve flash, 18/19 serve USB,
  // and 5/6/7/10/20/21 are used by this board's backlight and ST7789.
  return pin == 0 || pin == 1 || pin == 3 || pin == 4 || pin == 11;
#else
  (void)pin;
  return false;
#endif
}

bool touchPinValid() {
  return touchInputSupported() && touchPinAllowed(touchSettings.pin);
}

bool touchInputPressed() { return pinReady && stablePressed; }
const char* touchLastAction() { return lastAction; }
uint32_t touchEventCount() { return eventCount; }
uint32_t touchLastEventMs() { return lastEventMs; }

static void recordAction(const char* action) {
  lastAction = action;
  lastEventMs = millis();
  eventCount++;
  Serial.printf("Touch: %s\n", action);
}

static void cycleStyle(int8_t direction) {
  uint8_t mask = touchSettings.styleMask & STYLE_ACTIVE_MASK;
  if (mask == 0) mask = 1u << STYLE_DEFAULT;

  for (uint8_t step = 1; step <= STYLE_COUNT; step++) {
    int candidate = (int)displayStyle + direction * step;
    while (candidate < 0) candidate += STYLE_COUNT;
    candidate %= STYLE_COUNT;
    if (mask & (1u << candidate)) {
      displayStyle = (uint8_t)candidate;
      if (touchSettings.rememberStyle) saveDisplayStyle();
      ScreenState screen = getScreenState();
      if (screen == SCREEN_CLOCK) setScreenState(SCREEN_MONITOR);
      else forceDisplayRedraw();
      return;
    }
  }
}

static void runAction(uint8_t action, bool longPress) {
  switch (action) {
    case TOUCH_ACTION_NEXT_STYLE:
      cycleStyle(1);
      recordAction(longPress ? "hold: next layout" : "tap: next layout");
      break;
    case TOUCH_ACTION_PREV_STYLE:
      cycleStyle(-1);
      recordAction(longPress ? "hold: previous layout" : "tap: previous layout");
      break;
    case TOUCH_ACTION_TOGGLE_CLOCK: {
      ScreenState screen = getScreenState();
      if (screen == SCREEN_MONITOR) setScreenState(SCREEN_CLOCK);
      else if (screen == SCREEN_CLOCK) setScreenState(SCREEN_MONITOR);
      recordAction(longPress ? "hold: toggle clock" : "tap: toggle clock");
      break;
    }
    case TOUCH_ACTION_TOGGLE_POWER:
      if (displaySleepingForTouch()) wakeDisplayFromTouch();
      else setManualDisplayOff(true);
      recordAction(longPress ? "hold: toggle display" : "tap: toggle display");
      break;
    default:
      recordAction(longPress ? "hold: no action" : "tap: no action");
      break;
  }
}

void initTouchButton() {
  if (configuredPin != 0xFF) pinMode(configuredPin, INPUT);
  configuredPin = touchSettings.pin;
  pinReady = false;
  rawPressed = false;
  stablePressed = false;
  consumeUntilRelease = false;
  longActionFired = false;

  if (!touchSettings.enabled) {
    Serial.println("Touch: disabled");
    return;
  }
  if (!touchPinValid()) {
    Serial.printf("Touch: GPIO %u is not allowed on this board\n",
                  (unsigned)touchSettings.pin);
    return;
  }

  pinMode(configuredPin, touchSettings.activeHigh ? INPUT_PULLDOWN : INPUT_PULLUP);
  rawPressed = digitalRead(configuredPin) == (touchSettings.activeHigh ? HIGH : LOW);
  stablePressed = rawPressed;
  consumeUntilRelease = rawPressed;
  rawChangedMs = millis();
  pressedMs = rawChangedMs;
  pinReady = true;
  Serial.printf("Touch: GPIO %u, active %s\n", (unsigned)configuredPin,
                touchSettings.activeHigh ? "HIGH" : "LOW");
}

void handleTouchButton() {
  if (!pinReady) return;

  const uint32_t now = millis();
  const bool reading =
    digitalRead(configuredPin) == (touchSettings.activeHigh ? HIGH : LOW);
  if (reading != rawPressed) {
    rawPressed = reading;
    rawChangedMs = now;
  }

  if (reading != stablePressed && now - rawChangedMs >= TOUCH_DEBOUNCE_MS) {
    stablePressed = reading;
    if (stablePressed) {
      pressedMs = now;
      longActionFired = false;
      consumeUntilRelease = displaySleepingForTouch();
      if (consumeUntilRelease) {
        wakeDisplayFromTouch();
        recordAction("wake display");
      }
    } else {
      if (!consumeUntilRelease && !longActionFired)
        runAction(touchSettings.shortAction, false);
      consumeUntilRelease = false;
      longActionFired = false;
    }
  }

  if (stablePressed && !consumeUntilRelease && !longActionFired &&
      now - pressedMs >= TOUCH_LONG_PRESS_MS) {
    longActionFired = true;
    runAction(touchSettings.longAction, true);
  }
}
