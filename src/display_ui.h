#ifndef DISPLAY_UI_H
#define DISPLAY_UI_H

#include <LovyanGFX.hpp>

// Forward-declare the panel type so callers can use the pointer without
// pulling in the full header (which includes Arduino_GFX headers).
namespace lgfx { inline namespace v1 { class Panel_AXS15231B_AGFX; } }

// Screen states for the PC monitor. No printer/MQTT states - the device is a
// passive UDP receiver. SCREEN_MONITOR shows the gauge grid; SCREEN_CLOCK is
// the idle screen shown when the PC has gone offline (no UDP packets).
enum ScreenState {
  SCREEN_SPLASH,
  SCREEN_AP_MODE,
  SCREEN_CONNECTING_WIFI,
  SCREEN_WIFI_CONNECTED,
  SCREEN_MONITOR,
  SCREEN_CLOCK,
  SCREEN_OFF,
  SCREEN_OTA_UPDATE
};

extern lgfx::LovyanGFX* tft_ptr;
// Macro (NOT a reference) so callers' `tft.method()` always dereferences the
// current value of `tft_ptr`. On JC3248W535 we retarget this pointer to a
// PSRAM sprite at runtime; a C++ reference would have been permanently bound
// to the panel at static-init time, defeating the redirection.
#define tft (*tft_ptr)

// Direct pointer to the AXS15231B panel wrapper; only non-null on
// BOARD_IS_JC3248W535 builds.
extern lgfx::Panel_AXS15231B_AGFX* g_axs_panel;

void initDisplay();
void setBacklight(uint8_t level);
// Recompute the effective backlight from day/night scheduling and PC-offline
// sleep. Call updateBacklightControl() from the main loop.
void updateBacklightControl();
void refreshBacklightControl();
uint8_t currentBacklightLevel();
bool nightBrightnessActive();
bool offlineDisplaySleeping();
bool backlightTimeValid();
void applyDisplaySettings();  // re-apply rotation, bg, force redraw

// Renderer (renderer.cpp): screen-state machine + per-frame draw. updateDisplay()
// is throttled internally and safe to call every loop tick; it also redraws fully
// on a state change. setScreenState() marks a transition (forces a clean redraw).
void setScreenState(ScreenState state);
ScreenState getScreenState();
void updateDisplay();
// Force a clean full repaint on the next updateDisplay() without a state change
// (e.g. after the gauge mapping is edited in the portal). Safe from any context.
void forceDisplayRedraw();
// Bracket for the /screen.bmp capture render: while set, the renderer leaves
// the panel's pacing state (packet stamps, layout counts, smoothing, cached
// text widths) untouched so the capture never disturbs the live display.
void setCaptureRender(bool on);
// Tell the renderer the panel content was wiped (e.g. after a color change
// repaint) so styles repaint their static chrome on the next frame.
void markScreenCleared();
// Diagnostics: is the sparkline compose sprite allocated (flicker-free path),
// and how many allocation attempts have failed since boot.
bool sparkSpriteActive();
uint16_t sparkSpriteFails();
// Companion wobble diagnostics: raw per-frame changes seen vs debounced
// transitions the layout actually reacted to.
uint16_t rawNChanges();
uint16_t rawStatusChanges();
uint16_t acceptedRelayouts();
uint16_t acceptedStatusFlips();

// Flush the off-screen framebuffer sprite to the panel (JC3248W535 only; no-op
// elsewhere). Call once per loop tick after UI draws.
void flushFrame();
// Mark the framebuffer sprite dirty so the next flushFrame() pushes (JC3248W535
// only; no-op elsewhere).
void markFrameDirty();

#endif // DISPLAY_UI_H
