#ifndef CLOCK_MODE_H
#define CLOCK_MODE_H

// Draw the standard large idle clock. The live path keeps a small redraw
// cache; the snapshot path is stateless so web previews cannot disturb it.
void drawClock();
void drawClockSnapshot();
void resetClock();

#endif // CLOCK_MODE_H
