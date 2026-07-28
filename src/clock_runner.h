#ifndef CLOCK_RUNNER_H
#define CLOCK_RUNNER_H

// Color runner idle clock adapted from the read-only SmallOLED project. The
// live path animates minute changes; the snapshot path never mutates state.
void tickRunnerClock();
void drawRunnerClockSnapshot();
void resetRunnerClock();

#endif // CLOCK_RUNNER_H
