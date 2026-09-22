// Host-side tests for the sleep-screen arithmetic.
//
// Covers the two things that are easy to get quietly wrong and impossible to
// see without flashing: where an oversized wallpaper lands on screen, and which
// image the slideshow picks next.
//
// Run with test/run.sh.

#include "../src/sleep_layout.h"

#include <cstdio>
#include <initializer_list>
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

static SleepRecent recentOf(std::initializer_list<int> shown) {
  SleepRecent recent{};
  for (int index : shown) sleepScreenRemember(recent, static_cast<uint16_t>(index));
  return recent;
}

static void testSlideshow() {
  printf("slideshow order\n");
  const auto SLIDE = SleepScreenMode::SLIDESHOW;
  const auto SHUF  = SleepScreenMode::SHUFFLE;
  const SleepRecent none{};

  check(sleepScreenNextIndex(SLIDE, 1, true, none, 12345) == 0, "a single image is always index 0");
  check(sleepScreenNextIndex(SHUF,  1, true, none, 12345) == 0, "shuffle with one image too");
  check(sleepScreenNextIndex(SLIDE, 0, true, none, 12345) == 0, "an empty folder is index 0");

  check(sleepScreenNextIndex(SLIDE, 4, true, recentOf({0}), 0) == 1, "slideshow 0 -> 1");
  check(sleepScreenNextIndex(SLIDE, 4, true, recentOf({2}), 0) == 3, "slideshow 2 -> 3");
  check(sleepScreenNextIndex(SLIDE, 4, true, recentOf({3}), 0) == 0, "slideshow wraps 3 -> 0");

  check(sleepScreenNextIndex(SLIDE, 4, false, recentOf({5}), 0) == 0, "no history starts the slideshow at 0");
  check(sleepScreenNextIndex(SLIDE, 4, false, recentOf({1}), 0) == 0, "no history, another stale value");
  check(sleepScreenNextIndex(SLIDE, 3, false, recentOf({7}), 0) == 0, "no history, a third");
  {
    const int i = sleepScreenNextIndex(SHUF, 4, false, none, 7);
    check(i >= 0 && i < 4, "no history still produces a valid shuffle index");
  }

  {
    std::set<int> seen;
    SleepRecent recent{};
    int cur = 0;
    for (int i = 0; i < 5; i++) {
      cur = sleepScreenNextIndex(SLIDE, 5, true, recent, 0);
      sleepScreenRemember(recent, static_cast<uint16_t>(cur));
      seen.insert(cur);
    }
    check(seen.size() == 5, "a full slideshow cycle shows all 5 images");
  }

  // With a recent window, shuffle never returns an image still in that window.
  {
    bool inRange = true, repeated = false;
    for (uint32_t roll = 0; roll < 40; roll++) {
      for (int last = 0; last < 4; last++) {
        const int i = sleepScreenNextIndex(SHUF, 4, true, recentOf({last}), roll);
        if (i < 0 || i >= 4) inRange = false;
        if (i == last) repeated = true;
      }
    }
    check(inRange, "shuffle always lands inside the folder");
    check(!repeated, "shuffle never repeats an image still in the recent window");
  }

  // Three of four images already shown: every roll must land on the one left.
  {
    bool onlyTheMissing = true;
    const SleepRecent recent = recentOf({0, 2, 1});
    for (uint32_t roll = 0; roll < 20; roll++) {
      if (sleepScreenNextIndex(SHUF, 4, true, recent, roll) != 3) onlyTheMissing = false;
    }
    check(onlyTheMissing, "shuffle is forced onto the one image not yet shown");
  }

  check(sleepScreenNextIndex(SHUF, 2, true, recentOf({0}), 0) == 1, "with 2 images shuffle alternates (0 -> 1)");
  check(sleepScreenNextIndex(SHUF, 2, true, recentOf({1}), 1) == 0, "and back (1 -> 0)");

  {
    bool ok = true;
    for (uint32_t last = 0; last < 50; last++) {
      const SleepRecent recent = recentOf({static_cast<int>(last)});
      const int a = sleepScreenNextIndex(SLIDE, 3, true, recent, 0);
      const int b = sleepScreenNextIndex(SHUF,  3, true, recent, last * 7);
      if (a < 0 || a >= 3 || b < 0 || b >= 3) ok = false;
    }
    check(ok, "a stale index from a larger folder still lands in range");
  }
}

int main() {
  testFit();
  testSlideshow();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
