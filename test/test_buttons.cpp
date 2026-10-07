// The button ladder: the median filter in InputManager::getState() and the
// band table it feeds.
//
// This exists because the bug it covers cost five rounds of on-device
// instrumentation to find, and every round cost an SD flash and a reboot. The
// failure is entirely decidable on the host: the ladder turns an ADC number
// into a button, so a test that gets to choose the number gets to choose the
// failure.
//
// What was wrong on the device: phantom Right presses with nothing touching it.
// A second conversion of the same pin, microseconds later, read 4095 — full
// rest — so the voltage was fine and the conversion was not. One bad reading
// became a committed button press because getState() took a single sample and
// the band table has no "implausible" verdict: its bottom band is open to
// INT32_MIN, so anything low at all is a direction.
//
// Compiles the REAL lib/InputManager/src/InputManager.cpp against host stubs
// that let the test feed each conversion, so the test cannot drift from the
// code it checks.

#include <cstdio>
#include <vector>

// ARDOSIA_HOST_INPUT comes from run.sh: InputManager.cpp is a separate
// translation unit and would not see a #define made here.
#include "Arduino.h"
#include "driver/adc.h"
#include "InputManager.h"

static int failures = 0;

static void check(const bool ok, const char* what) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static void checkEq(const int got, const int want, const char* what) {
  if (got == want) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n        got  %d\n        want %d\n", what, got, want);
  }
}

// The values a real X4 reports, from the table at the top of InputManager.cpp.
static constexpr int AT_REST = 4095;
static constexpr int V_BACK = 3512;
static constexpr int V_CONFIRM = 2694;
static constexpr int V_LEFT = 1493;
static constexpr int V_RIGHT = 5;
static constexpr int V_UP = 2242;
static constexpr int V_DOWN = 5;

// One poll of the input manager at a chosen instant. Channel 1 and channel 2
// are fed separately because they are separate pins: getState() reads three
// conversions of each, in that order.
static void poll(InputManager& im, const std::vector<int>& ch1,
                 const std::vector<int>& ch2, const unsigned long dtMs) {
  hostAdvanceMillis(dtMs);
  adcFeed(ADC1_CHANNEL_1, ch1);
  adcFeed(ADC1_CHANNEL_2, ch2);
  im.update();
}

// The committed state, read WITHOUT polling the ADC again. getState() consumes
// conversions, so calling it to assert would take three more reads past the end
// of the fed queue — where the last value repeats — and decide the test on
// samples the code under test never saw.
static uint8_t committedState(const InputManager& im) {
  uint8_t s = 0;
  for (uint8_t b = 0; b <= InputManager::BTN_POWER; b++) {
    if (im.isPressed(b)) s |= static_cast<uint8_t>(1 << b);
  }
  return s;
}

// A steady pin: every conversion returns the same thing.
static std::vector<int> steady(const int v) { return {v, v, v}; }

// Hold a value long enough for the debounce to commit it, at the given poll
// cadence. The firmware polls every 10 ms while recently active and every
// 50 ms idle (the delay at the foot of loop()), and DEBOUNCE_DELAY is 5 ms, so
// two agreeing polls commit at either cadence.
static void settle(InputManager& im, const int ch1, const int ch2,
                   const unsigned long cadence, const int polls = 3) {
  for (int i = 0; i < polls; i++) poll(im, steady(ch1), steady(ch2), cadence);
}

// ---------------------------------------------------------------------------

static void testRealPressesStillWork() {
  printf("every real press still arrives\n");

  struct Row { int ch1, ch2, button; const char* name; };
  const Row rows[] = {
      {V_BACK,  AT_REST, InputManager::BTN_BACK,    "Back"},
      {V_CONFIRM, AT_REST, InputManager::BTN_CONFIRM, "Confirm"},
      {V_LEFT,  AT_REST, InputManager::BTN_LEFT,    "Left"},
      {V_RIGHT, AT_REST, InputManager::BTN_RIGHT,   "Right"},
      {AT_REST, V_UP,    InputManager::BTN_UP,      "Up"},
      {AT_REST, V_DOWN,  InputManager::BTN_DOWN,    "Down"},
  };

  // Both cadences: a sample-count rule would have passed at 10 ms and failed
  // at 50 ms, where a 100 ms press is only two polls. That is the trap the fix
  // had to avoid, so both are swept.
  const unsigned long cadences[] = {10, 50};

  for (const unsigned long cadence : cadences) {
    for (const Row& r : rows) {
      InputManager im;
      im.begin();
      settle(im, AT_REST, AT_REST, cadence);
      settle(im, r.ch1, r.ch2, cadence);
      char what[64];
      snprintf(what, sizeof(what), "%s commits at a %lums cadence", r.name, cadence);
      check(im.isPressed(r.button), what);
    }
  }

  // And nothing is pressed when nothing is pressed.
  InputManager im;
  im.begin();
  settle(im, AT_REST, AT_REST, 10);
  checkEq(committedState(im), 0, "rest reports no button at all");
}

static void testOneBadConversionIsRejected() {
  printf("a single bad conversion never becomes a press\n");

  // The measured device failure: channel 1 reads the floor once, then rest.
  // Before the median this committed Right; the whole bug in one line.
  for (int position = 0; position < 3; position++) {
    InputManager im;
    im.begin();
    settle(im, AT_REST, AT_REST, 10);

    std::vector<int> ch1 = steady(AT_REST);
    ch1[position] = V_RIGHT;

    // Two consecutive polls, because the debounce needs two agreeing reads to
    // commit — this is exactly the shape the device produced (d20-22, a
    // two-sample phantom at the 10 ms cadence).
    poll(im, ch1, steady(AT_REST), 10);
    poll(im, ch1, steady(AT_REST), 10);

    char what[80];
    snprintf(what, sizeof(what),
             "bad read in slot %d of 3 does not commit Right", position);
    check(!im.isPressed(InputManager::BTN_RIGHT), what);
    snprintf(what, sizeof(what), "bad read in slot %d leaves state clean", position);
    checkEq(committedState(im), 0, what);
  }

  // The same on channel 2, where a low reading is Down and a mid one is Up.
  for (const int bad : {V_DOWN, V_UP}) {
    for (int position = 0; position < 3; position++) {
      InputManager im;
      im.begin();
      settle(im, AT_REST, AT_REST, 10);

      std::vector<int> ch2 = steady(AT_REST);
      ch2[position] = bad;
      poll(im, steady(AT_REST), ch2, 10);
      poll(im, steady(AT_REST), ch2, 10);

      char what[96];
      snprintf(what, sizeof(what),
               "channel 2 bad read %d in slot %d does not commit", bad, position);
      checkEq(committedState(im), 0, what);
    }
  }
}

static void testRealPressSurvivesOneHighOutlier() {
  printf("a real press survives one spuriously HIGH conversion\n");

  // The case that makes this a MEDIAN and not a maximum. Mutation found the
  // gap: replacing the median with max(a, b) passed every other check here,
  // because every other check feeds a LOW outlier and a maximum rejects those
  // just as well. It differs only when a button is genuinely held and one
  // conversion comes back at rest — where a maximum drops the press and a
  // median keeps it.
  //
  // Worth covering rather than assuming one-sided failure: nothing measured on
  // the device says a bad conversion can only read low. It says the ones that
  // caused phantoms read low, which is a different claim.
  for (int position = 0; position < 3; position++) {
    InputManager im;
    im.begin();
    settle(im, AT_REST, AT_REST, 10);

    std::vector<int> ch1 = steady(V_RIGHT);
    ch1[position] = AT_REST;
    poll(im, ch1, steady(AT_REST), 10);
    poll(im, ch1, steady(AT_REST), 10);

    char what[88];
    snprintf(what, sizeof(what),
             "held Right survives a rest read in slot %d of 3", position);
    check(im.isPressed(InputManager::BTN_RIGHT), what);
  }

  // And the same on channel 2, for Up — whose band is the wide one.
  for (int position = 0; position < 3; position++) {
    InputManager im;
    im.begin();
    settle(im, AT_REST, AT_REST, 10);

    std::vector<int> ch2 = steady(V_UP);
    ch2[position] = AT_REST;
    poll(im, steady(AT_REST), ch2, 10);
    poll(im, steady(AT_REST), ch2, 10);

    char what[88];
    snprintf(what, sizeof(what),
             "held Up survives a rest read in slot %d of 3", position);
    check(im.isPressed(InputManager::BTN_UP), what);
  }
}

static void testTwoBadOfThreeStillGetsThrough() {
  printf("the filter's limit, stated rather than discovered later\n");

  // A median of three survives ONE bad reading per poll, not two. This is not
  // a defect to fix here, it is the filter's documented ceiling: if the device
  // ever shows r rising again with the median in place, this is why, and the
  // answer is a median of five or discarding the first read — not a longer
  // debounce, which the measurements already ruled out.
  InputManager im;
  im.begin();
  settle(im, AT_REST, AT_REST, 10);

  const std::vector<int> twoBad = {V_RIGHT, V_RIGHT, AT_REST};
  poll(im, twoBad, steady(AT_REST), 10);
  poll(im, twoBad, steady(AT_REST), 10);
  check(im.isPressed(InputManager::BTN_RIGHT),
        "two bad of three still commits — the ceiling is one per poll");
}

static void testFilterCounterIsHonest() {
  printf("the filter counter only moves when it has something to reject\n");

  // On the device this number is what separates "the fix works" from "the
  // phantom happened to stop": r staying 0 proves nothing on its own.
  InputManager im;
  im.begin();
  settle(im, AT_REST, AT_REST, 10);

  const uint32_t quiet = InputManager::getFilteredSampleCount();
  settle(im, AT_REST, AT_REST, 10, 5);
  checkEq(static_cast<int>(InputManager::getFilteredSampleCount() - quiet), 0,
          "agreeing conversions never bump the counter");

  std::vector<int> ch1 = steady(AT_REST);
  ch1[0] = V_RIGHT;
  poll(im, ch1, steady(AT_REST), 10);
  check(InputManager::getFilteredSampleCount() > quiet,
        "a disagreeing poll does bump it");
}

static void testBandTableShape() {
  printf("the band table's shape, including what it does with nonsense\n");

  // getButtonFromADC is private, so this goes through getState() — which is
  // the honest level anyway: what matters is which BUTTON a number becomes.
  struct Row { int v; int state; const char* name; };
  const Row rows[] = {
      {4095, 0, "4095 is no button"},
      {3801, 0, "3801 is no button — the rest band is only 3801..4095"},
      {3800, 1 << InputManager::BTN_BACK, "3800 is already Back"},
      {750,  1 << InputManager::BTN_RIGHT, "750 is Right"},
      {0,    1 << InputManager::BTN_RIGHT, "zero is Right"},
      // The latent bug: the bottom band is open to INT32_MIN, so a negative
      // reading is a Right press. adc1_get_raw() has no path that returns one
      // on the C3 today — it blocks for the lock rather than failing, and the
      // single read busy-waits on the DONE flag — so this is not reachable.
      // Pinned anyway: any future error return becomes a silent Right, and a
      // test is the cheapest way to make that change announce itself.
      {-1,   1 << InputManager::BTN_RIGHT, "-1 decodes as Right (latent)"},
  };

  for (const Row& r : rows) {
    InputManager im;
    im.begin();
    settle(im, AT_REST, AT_REST, 10);
    settle(im, r.v, AT_REST, 10);
    checkEq(committedState(im), r.state, r.name);
  }

  // The rest band is 295 counts out of 4096 — about 7% of the range, and the
  // reason a reading that merely comes back low is always a direction. Stated
  // as a test so that narrowing it is a deliberate act.
  InputManager im;
  im.begin();
  settle(im, AT_REST, AT_REST, 10);
  settle(im, 3900, AT_REST, 10);
  checkEq(committedState(im), 0, "3900 is still rest");
}

int main() {
  printf("\n=== button ladder ===\n\n");
  testRealPressesStillWork();
  printf("\n");
  testOneBadConversionIsRejected();
  printf("\n");
  testRealPressSurvivesOneHighOutlier();
  printf("\n");
  testTwoBadOfThreeStillGetsThrough();
  printf("\n");
  testFilterCounterIsHonest();
  printf("\n");
  testBandTableShape();
  printf("\n");

  if (failures) {
    printf("FAILURES: %d\n", failures);
    return 1;
  }
  printf("button ladder: every check passed\n");
  return 0;
}
