// Host-side tests for the direction-button rotation.
//
// A rotation table is easy to write and easy to get subtly wrong — two buttons
// landing on the same direction, one direction unreachable, a landscape that is
// its own inverse. None of that is visible by reading it; all of it is one line
// to assert. On a device that costs an SD card write per attempt, that matters.
//
// What these do NOT check is which of GfxRenderer's two landscape values turns
// the panel which way: that is a fact about the hardware, not about the table.
// They check the shape, so a swap is the only thing left to discover by eye.
//
// Run with test/run.sh.

#include "../src/dpad.h"

#include <cstdio>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
  checks++;
  if (!ok) {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static const Orientation kOrientations[] = {
    Orientation::PORTRAIT, Orientation::LANDSCAPE_CW,
    Orientation::PORTRAIT_INV, Orientation::LANDSCAPE_CCW};

static const Dir kDirs[] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};

static const char* dirName(Dir d) {
  switch (d) {
    case Dir::Up: return "Up";
    case Dir::Down: return "Down";
    case Dir::Left: return "Left";
    case Dir::Right: return "Right";
    default: return "?";
  }
}

// ---------------------------------------------------------------------------

static void testFixedIsIdentity() {
  printf("dpad: Fixed leaves the buttons alone\n");

  // Fixed is what the firmware has always done. If it ever stops being the
  // identity, every existing muscle memory breaks in whichever orientation the
  // user happens to be in.
  for (Orientation o : kOrientations) {
    for (Dir d : kDirs) {
      check(dpadResolve(d, o, DpadMode::Fixed) == d,
            "Fixed returns the button pressed, whatever the screen is doing");
    }
  }
}

static void testPortraitIsIdentity() {
  printf("dpad: Natural is a no-op in portrait\n");

  // Portrait is the unrotated case, so Natural has nothing to do there. If this
  // failed, turning the setting on would scramble the buttons for anyone who
  // never leaves portrait.
  for (Dir d : kDirs) {
    check(dpadResolve(d, Orientation::PORTRAIT, DpadMode::Natural) == d,
          "portrait maps every button to itself");
  }
}

static void testEveryOrientationIsABijection() {
  printf("dpad: no button is lost and none is doubled\n");

  // The failure this catches is the one that reads fine: two physical buttons
  // resolving to the same direction, which silently makes a third direction
  // unreachable.
  for (Orientation o : kOrientations) {
    bool seen[4] = {false, false, false, false};
    for (Dir d : kDirs) {
      const Dir r = dpadResolve(d, o, DpadMode::Natural);
      check(r != Dir::Count, "a real button never resolves to Count");
      const int i = static_cast<int>(r);
      if (i >= 0 && i < 4) {
        if (seen[i]) {
          printf("  FAIL  orientation %d sends two buttons to %s\n",
                 static_cast<int>(o), dirName(r));
          failures++;
        }
        checks++;
        seen[i] = true;
      }
    }
    for (int i = 0; i < 4; i++) {
      check(seen[i], "every direction is reachable from some button");
    }
  }
}

static void testInverseRelationships() {
  printf("dpad: the rotations relate the way rotations must\n");

  // Inverted portrait is a half turn: doing it twice is doing nothing.
  for (Dir d : kDirs) {
    const Dir once = dpadResolve(d, Orientation::PORTRAIT_INV, DpadMode::Natural);
    const Dir twice = dpadResolve(once, Orientation::PORTRAIT_INV, DpadMode::Natural);
    check(twice == d, "inverted portrait applied twice is the identity");
  }

  // The two landscapes are quarter turns in opposite directions, so each undoes
  // the other. This is what catches a table where one landscape was copied from
  // the other and edited halfway.
  for (Dir d : kDirs) {
    const Dir cw = dpadResolve(d, Orientation::LANDSCAPE_CW, DpadMode::Natural);
    const Dir back = dpadResolve(cw, Orientation::LANDSCAPE_CCW, DpadMode::Natural);
    check(back == d, "one landscape undoes the other");
  }

  // And a quarter turn is not a half turn: applying the same landscape twice
  // must invert, not restore.
  for (Dir d : kDirs) {
    const Dir once = dpadResolve(d, Orientation::LANDSCAPE_CW, DpadMode::Natural);
    const Dir twice = dpadResolve(once, Orientation::LANDSCAPE_CW, DpadMode::Natural);
    check(twice != d, "a landscape is a quarter turn, not a half one");
    check(twice == dpadResolve(d, Orientation::PORTRAIT_INV, DpadMode::Natural),
          "two quarter turns make the half turn");
  }
}

static void testTheDeviceVerifiedTable() {
  printf("dpad: the table the device agreed with\n");

  // This started out pinned to the old `|| btnRight` patch — every list screen
  // read "Up OR Right moves up", so the case that patch was written for had its
  // right-hand button pointing up the page. That turned out to be evidence
  // about the author's case and not about this one: Ardosia is held with the
  // bottom edge to the RIGHT and the side keys above, and on the panel the
  // mapping came out exactly reversed. The two landscape cases were exchanged
  // on 2026-09-23 and this test was turned round with them.
  //
  // So these four are not derived from anything — they ARE the device evidence,
  // written down. Everything else in this file is shape (bijection, mirrors,
  // identity) and would pass just as happily with the table reversed, which is
  // precisely why this one has to be concrete.
  check(dpadResolve(Dir::Left, Orientation::LANDSCAPE_CW, DpadMode::Natural) == Dir::Up,
        "bottom edge to the right: the left-hand button points up the page");
  check(dpadResolve(Dir::Right, Orientation::LANDSCAPE_CW, DpadMode::Natural) == Dir::Down,
        "and the right-hand button points down");
  check(dpadResolve(Dir::Up, Orientation::LANDSCAPE_CW, DpadMode::Natural) == Dir::Right,
        "the button labelled Up points along the page");
  check(dpadResolve(Dir::Down, Orientation::LANDSCAPE_CW, DpadMode::Natural) == Dir::Left,
        "and the one labelled Down points back");

  // The other landscape is the mirror, and it is the one the old patch
  // described. Pinned too, so a future edit cannot quietly make both landscapes
  // the same rotation — which would still pass every shape test in this file.
  check(dpadResolve(Dir::Right, Orientation::LANDSCAPE_CCW, DpadMode::Natural) == Dir::Up,
        "the other way up, the right-hand button points up, as the old patch had it");
  check(dpadResolve(Dir::Left, Orientation::LANDSCAPE_CCW, DpadMode::Natural) == Dir::Down,
        "and the left-hand button points down");
}

int main() {
  printf("\n=== d-pad rotation tests ===\n\n");

  testFixedIsIdentity();
  testPortraitIsIdentity();
  testEveryOrientationIsABijection();
  testInverseRelationships();
  testTheDeviceVerifiedTable();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
