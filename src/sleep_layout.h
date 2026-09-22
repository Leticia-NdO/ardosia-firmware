#pragma once

// The arithmetic behind the sleep screen, with no SD card and no renderer in
// it, so test/run.sh can exercise it on a host compiler. Same split as
// keymap/deadkeys: on a device with no serial console, logic that cannot be
// tested off-device cannot be tested at all.

#include "config.h"
#include <cstdint>

// Where a bitmap lands on screen, already scaled down to fit and centred.
struct SleepPlacement {
  int x;
  int y;
  int w;   // drawn size, after scaling
  int h;
};

// Images larger than the screen are scaled down to fit and centred. Smaller
// images are never scaled up — enlarging a 1-bit source on e-ink turns it
// to mush.
SleepPlacement sleepScreenFit(int bitmapW, int bitmapH, int screenW, int screenH);

// How many recently shown indices the shuffle remembers. Same window CrossInk
// keeps in CrossPointState::SLEEP_RECENT_COUNT.
static constexpr int SLEEP_RECENT_CAP = 16;

// Circular buffer of recently shown image indices. pos is the next write
// slot, fill is how many entries are valid (0..SLEEP_RECENT_CAP).
struct SleepRecent {
  uint16_t indices[SLEEP_RECENT_CAP];
  uint8_t pos;
  uint8_t fill;
};

// Slideshow: next in order, wrapping. Cold boot (no history) starts at 0.
// Shuffle: uniform among images that are not in the recent window, so a
// folder is walked before anything repeats. The window is capped at count-1
// so a small folder still has somewhere to go. `roll` is the random draw,
// passed in so the host tests stay deterministic.
int sleepScreenNextIndex(SleepScreenMode mode, int count, bool haveHistory,
                         const SleepRecent& recent, uint32_t roll);

void sleepScreenRemember(SleepRecent& recent, uint16_t index);
