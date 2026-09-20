// Host-side tests for the sleep-screen arithmetic.
//
// Covers the two things that are easy to get quietly wrong and impossible to
// see without flashing: where an oversized wallpaper lands on screen, and which
// image the slideshow picks next.
//
// Run with test/run.sh.

#include "../src/sleep_layout.h"

#include <cstdio>
#include <set>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
  checks++;
  if (!ok) { failures++; printf("  FAIL  %s\n", what); }
}

static void checkPlacement(SleepPlacement p, int x, int y, int w, int h, const char* what) {
  checks++;
  if (p.x != x || p.y != y || p.w != w || p.h != h) {
    failures++;
    printf("  FAIL  %s\n        got  x=%d y=%d %dx%d\n        want x=%d y=%d %dx%d\n",
           what, p.x, p.y, p.w, p.h, x, y, w, h);
  }
}

// The X4 panel, in both orientations.
static const int PW = 480, PH = 800;   // portrait
static const int LW = 800, LH = 480;   // landscape

static void testFit() {
  printf("wallpaper placement\n");

  // Exact fit: no scaling, no offset.
  checkPlacement(sleepScreenFit(480, 800, PW, PH), 0, 0, 480, 800, "a 480x800 image fills the portrait screen");
  checkPlacement(sleepScreenFit(800, 480, LW, LH), 0, 0, 800, 480, "a 800x480 image fills the landscape screen");

  // Smaller than the screen: centred, never scaled up. Scaling up on e-ink
  // turns a crisp 1-bit image into mush, so it is deliberately not done.
  checkPlacement(sleepScreenFit(240, 400, PW, PH), 120, 200, 240, 400, "a small image is centred, not enlarged");
  checkPlacement(sleepScreenFit(100, 100, PW, PH), 190, 350, 100, 100, "a square thumbnail is centred both ways");

  // Wider than the screen: scaled to the width, letterboxed vertically.
  checkPlacement(sleepScreenFit(960, 1600, PW, PH), 0, 0, 480, 800, "an exact 2x image scales to fill");
  checkPlacement(sleepScreenFit(1920, 1080, PW, PH), 0, 265, 480, 270,
                 "a 16:9 photo on the portrait screen is letterboxed top and bottom");

  // Taller than the screen: scaled to the height, pillarboxed horizontally.
  checkPlacement(sleepScreenFit(1080, 1920, LW, LH), 265, 0, 270, 480,
                 "a tall photo on the landscape screen is pillarboxed left and right");

  // The regression this function exists to prevent: centring against the file's
  // size instead of the scaled size pushes the image off the bottom.
  {
    const SleepPlacement p = sleepScreenFit(1600, 2400, PW, PH);
    check(p.y >= 0 && p.y + p.h <= PH, "a scaled portrait wallpaper stays on screen vertically");
    check(p.x >= 0 && p.x + p.w <= PW, "and horizontally");
    check(p.w == 480, "it is scaled to the screen width");
  }

  // Degenerate inputs must not produce a draw.
  checkPlacement(sleepScreenFit(0, 0, PW, PH), 0, 0, 0, 0, "a zero-sized bitmap draws nothing");
  checkPlacement(sleepScreenFit(-5, 100, PW, PH), 0, 0, 0, 0, "a negative width draws nothing");
  checkPlacement(sleepScreenFit(100, 100, 0, 0), 0, 0, 0, 0, "a zero-sized screen draws nothing");
}

static void testSlideshow() {
  printf("slideshow order\n");
  const auto SLIDE = SleepScreenMode::SLIDESHOW;
  const auto SHUF  = SleepScreenMode::SHUFFLE;

  // One image, or none: always index 0, whatever the history says.
  check(sleepScreenNextIndex(SLIDE, 1, true, 0, 12345) == 0, "a single image is always index 0");
  check(sleepScreenNextIndex(SHUF,  1, true, 0, 12345) == 0, "shuffle with one image too");
  check(sleepScreenNextIndex(SLIDE, 0, true, 0, 12345) == 0, "an empty folder is index 0");

  // Sequential: advances one and wraps.
  check(sleepScreenNextIndex(SLIDE, 4, true, 0, 0) == 1, "slideshow 0 -> 1");
  check(sleepScreenNextIndex(SLIDE, 4, true, 2, 0) == 3, "slideshow 2 -> 3");
  check(sleepScreenNextIndex(SLIDE, 4, true, 3, 0) == 0, "slideshow wraps 3 -> 0");

  // Cold boot (RTC not valid) starts at the beginning rather than at garbage.
  //
  // The `last` values here are chosen so that (last + 1) % count is NOT 0: with
  // an unlucky value the "ignore the history" bug returns 0 anyway and the test
  // passes for the wrong reason. (Found by mutation testing, which is the only
  // reason this comment exists.)
  check(sleepScreenNextIndex(SLIDE, 4, false, 5, 0) == 0, "no history starts the slideshow at 0");
  check(sleepScreenNextIndex(SLIDE, 4, false, 1, 0) == 0, "no history, another stale value");
  check(sleepScreenNextIndex(SLIDE, 3, false, 7, 0) == 0, "no history, a third");
  {
    const int i = sleepScreenNextIndex(SHUF, 4, false, 99999, 7);
    check(i >= 0 && i < 4, "no history still produces a valid shuffle index");
  }

  // A full cycle visits every image exactly once.
  {
    std::set<int> seen;
    int cur = 0;
    for (int i = 0; i < 5; i++) { cur = sleepScreenNextIndex(SLIDE, 5, true, cur, 0); seen.insert(cur); }
    check(seen.size() == 5, "a full slideshow cycle shows all 5 images");
  }

  // Shuffle stays in range for every possible draw, and never repeats the last.
  {
    bool inRange = true, repeated = false;
    for (uint32_t roll = 0; roll < 200; roll++) {
      for (int last = 0; last < 4; last++) {
        const int i = sleepScreenNextIndex(SHUF, 4, true, (uint32_t)last, roll);
        if (i < 0 || i >= 4) inRange = false;
        if (i == last) repeated = true;
      }
    }
    check(inRange, "shuffle always lands inside the folder");
    check(!repeated, "shuffle never shows the same wallpaper twice running");
  }

  // Shuffle with two images degenerates to alternating - that is correct, not a bug.
  check(sleepScreenNextIndex(SHUF, 2, true, 0, 0) == 1, "with 2 images shuffle alternates (0 -> 1)");
  check(sleepScreenNextIndex(SHUF, 2, true, 1, 1) == 0, "and back (1 -> 0)");

  // A stale RTC index larger than the folder must not index out of bounds:
  // images get deleted from the card between sleeps.
  {
    bool ok = true;
    for (uint32_t last = 0; last < 50; last++) {
      const int a = sleepScreenNextIndex(SLIDE, 3, true, last, 0);
      const int b = sleepScreenNextIndex(SHUF,  3, true, last, last * 7);
      if (a < 0 || a >= 3 || b < 0 || b >= 3) ok = false;
    }
    check(ok, "a stale index from a larger folder still lands in range");
  }
}

// Black pixels in one 4x4 tile of the dither pattern.
static int tileBlackCount(uint8_t level, SleepBrightness b) {
  int n = 0;
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++)
      if (sleepScreenPixelIsBlack(level, x, y, b)) n++;
  return n;
}

static void testHalftone() {
  printf("wallpaper halftone\n");
  const auto N = SleepBrightness::NORMAL;
  const auto L = SleepBrightness::LIGHT;
  const auto X = SleepBrightness::LIGHTER;

  // White stays paper-white and black stays solid, at every setting.
  // Brightening must lift the mid tones, not grey out the page or punch holes
  // in the blacks.
  for (auto b : {N, L, X}) {
    check(tileBlackCount(3, b) == 0,  "level 3 (white) is never painted");
    check(tileBlackCount(0, b) == 16, "level 0 (black) is always painted");
  }

  // The bug this replaces: drawBitmap in BW mode painted levels 1 AND 2 fully
  // black, so anything below 55% brightness was a slab. They are now tones.
  check(tileBlackCount(1, N) > 0 && tileBlackCount(1, N) < 16, "level 1 is a tone, not solid black");
  check(tileBlackCount(2, N) > 0 && tileBlackCount(2, N) < 16, "level 2 is a tone, not solid black");
  check(tileBlackCount(2, N) < tileBlackCount(1, N), "the lighter level is the lighter tone");

  // Brightening actually brightens, and does so monotonically.
  check(tileBlackCount(2, L) < tileBlackCount(2, N), "Light lifts the mid tone above Normal");
  check(tileBlackCount(2, X) < tileBlackCount(2, L), "Lighter lifts it again");
  check(tileBlackCount(1, L) < tileBlackCount(1, N), "and the dark tone too");
  check(tileBlackCount(1, X) < tileBlackCount(1, L), "and again");

  // The density the curve promises is the density that gets painted — this is
  // what proves the Bayer matrix is a permutation of 0..15 and not lumpy.
  bool exact = true;
  for (auto b : {N, L, X})
    for (uint8_t level = 0; level <= 3; level++)
      if (tileBlackCount(level, b) != sleepScreenLevelDensity(level, b)) exact = false;
  check(exact, "every tile paints exactly the density the tone curve asks for");

  // Ordered dithering must SPREAD the dots. The first version of this test
  // demanded an equal count per row and failed: a Bayer matrix does not promise
  // that (at density 12 it lays out 4,2,4,2 per row, which is correct). What
  // actually matters is that no tone collapses into a line or a corner blob.
  {
    bool dispersed = true;
    for (auto b : {N, L, X}) {
      for (uint8_t level = 1; level <= 2; level++) {
        const int total = sleepScreenLevelDensity(level, b);
        if (total <= 0 || total >= 16) continue;

        int perRow[4] = {0, 0, 0, 0};
        int perCol[4] = {0, 0, 0, 0};
        for (int y = 0; y < 4; y++)
          for (int x = 0; x < 4; x++)
            if (sleepScreenPixelIsBlack(level, x, y, b)) { perRow[y]++; perCol[x]++; }

        int rowsUsed = 0, colsUsed = 0, maxRow = 0, minRow = 4;
        for (int i = 0; i < 4; i++) {
          if (perRow[i] > 0) rowsUsed++;
          if (perCol[i] > 0) colsUsed++;
          if (perRow[i] > maxRow) maxRow = perRow[i];
          if (perRow[i] < minRow) minRow = perRow[i];
        }
        // At least two rows and two columns carry dots, and no row is full
        // while another is empty.
        if (rowsUsed < 2 || colsUsed < 2 || (maxRow - minRow) > 2) dispersed = false;
      }
    }
    check(dispersed, "every tone spreads across rows and columns, never a line or a blob");
  }

  // Ordered dithering must also be NESTED: the dots of a lighter tone are a
  // subset of the dots of a darker one. Without that, two adjacent tones can
  // use disjoint dot positions and the boundary between them shimmers.
  {
    bool nested = true;
    // The tones the settings can produce are ordered, so their dot sets nest.
    for (int y = 0; y < 4; y++) {
      for (int x = 0; x < 4; x++) {
        const bool lighter = sleepScreenPixelIsBlack(2, x, y, X);
        const bool light   = sleepScreenPixelIsBlack(2, x, y, L);
        const bool normal  = sleepScreenPixelIsBlack(2, x, y, N);
        const bool darker  = sleepScreenPixelIsBlack(1, x, y, N);
        if (lighter && !light) nested = false;
        if (light && !normal)  nested = false;
        if (normal && !darker) nested = false;
      }
    }
    check(nested, "lighter tones use a subset of the darker tones' dots");
  }

  // The pattern must repeat every 4 pixels, including far from the origin,
  // so a wallpaper placed at an odd offset still tiles cleanly.
  {
    bool tiles = true;
    for (uint8_t level = 0; level <= 3; level++)
      for (int x = 0; x < 9; x++)
        for (int y = 0; y < 9; y++)
          if (sleepScreenPixelIsBlack(level, x, y, N) !=
              sleepScreenPixelIsBlack(level, x + 4, y + 4, N)) tiles = false;
    check(tiles, "the dither pattern tiles every 4 pixels");
  }

  // Out-of-range input must not index off the table.
  check(!sleepScreenPixelIsBlack(7, 0, 0, N), "an impossible level paints nothing");
  check(sleepScreenLevelDensity(9, N) == 0, "an impossible level has no density");
}

int main() {
  testFit();
  testSlideshow();
  testHalftone();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
