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

// drawBitmap() scales an oversized image down to the screen but places it from
// the top-left corner, so centring must be computed against the SCALED size,
// not the file's. Images smaller than the screen are never scaled up.
SleepPlacement sleepScreenFit(int bitmapW, int bitmapH, int screenW, int screenH);

// ---------------------------------------------------------------------------
// Tone mapping
//
// The panel is 1-bit: a pixel is black or white, there is no grey. Bitmap's
// reader hands back 2 bits per pixel (0 black, 1 dark, 2 light, 3 white), and
// GfxRenderer::drawBitmap() in BW mode collapses that by painting EVERYTHING
// below pure white as solid black. Since the quantiser's top threshold is 140
// of 255, any pixel dimmer than 55% brightness comes out black — which is why
// wallpapers looked almost entirely dark.
//
// So the levels are rendered as HALFTONE instead: an ordered 4x4 Bayer pattern
// whose dot density comes from the level. Mid tones become a stipple the eye
// reads as grey, the way newsprint does, rather than a slab of black.
// ---------------------------------------------------------------------------

// Should this pixel be painted black?
//   level  0..3 from Bitmap::readNextRow (0 = black ... 3 = white)
//   x, y   SCREEN coordinates, so the pattern stays regular after scaling
bool sleepScreenPixelIsBlack(uint8_t level, int x, int y, SleepBrightness brightness);

// Dot density for a level, 0 (never black) to 16 (always black).
// Exposed so the tests can state the tone curve directly.
int sleepScreenLevelDensity(uint8_t level, SleepBrightness brightness);

// Which wallpaper this sleep shows.
//   haveHistory  false on a cold boot, when the RTC counter is not valid
//   last         the index shown last time
//   roll         a random draw, passed in so this stays deterministic in tests
int sleepScreenNextIndex(SleepScreenMode mode, int count,
                         bool haveHistory, uint32_t last, uint32_t roll);
