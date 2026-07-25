#ifndef LED_H
#define LED_H

#include <Arduino.h>

// Status LED: one plain single-color LED on a GPIO, dimmed with PWM through a
// transistor driver. NOT addressable - there is no color to set, only
// brightness. Ported from the BambuHelper LED module; its printer-state
// effects (finish pulse, pause breathing, error strobe) describe a machine
// this firmware does not talk to, so they were left out rather than
// repurposed into something invented.
//
// The LED follows the schedule the panel already has: a separate night level
// during the configured night interval, and dark whenever the display itself
// is dark (manual off, PC-offline sleep, or a night brightness of 0).

// Lifecycle. initLed() re-reads ledSettings, so the save handler calls it to
// apply a changed pin or brightness. ledTick() must run every loop pass.
void initLed();
void ledTick();
void shutdownLed();

// Portal live preview: drive the LED from unsaved form values so the slider
// moves the real LED. NVS is untouched; the next initLed() clears it.
void previewLed(bool enabled, uint8_t pin, uint8_t brightness);
void clearLedPreview();

// Pin validation. sanitizeLedPin() runs on load and before every NVS write.
bool ledPinAllowed(uint8_t pin);
bool ledPinValid();
void sanitizeLedPin();

// Live state for /api/status.
uint8_t ledCurrentDuty();

#endif // LED_H
