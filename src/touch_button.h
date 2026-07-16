#ifndef TOUCH_BUTTON_H
#define TOUCH_BUTTON_H

#include <Arduino.h>

void initTouchButton();
void handleTouchButton();

bool touchInputSupported();
bool touchPinAllowed(uint8_t pin);
bool touchPinValid();
bool touchInputPressed();
const char* touchLastAction();
uint32_t touchEventCount();
uint32_t touchLastEventMs();

#endif // TOUCH_BUTTON_H
