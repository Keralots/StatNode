#ifndef CLOCK_MARIO_H
#define CLOCK_MARIO_H

// Color Mario idle clock adapted from the read-only SmallOLED project. The
// live path animates minute changes; the snapshot path never mutates state.
void tickMarioClock();
void drawMarioClockSnapshot();
void resetMarioClock();

#endif // CLOCK_MARIO_H
