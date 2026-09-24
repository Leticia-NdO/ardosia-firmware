#include "dpad.h"

Dir dpadResolve(const Dir physical, const Orientation orientation, const DpadMode mode) {
  if (physical == Dir::Count) return Dir::Count;
  if (mode == DpadMode::Fixed) return physical;

  switch (orientation) {
    case Orientation::PORTRAIT:
      return physical;

    case Orientation::PORTRAIT_INV:
      // Upside down: every direction becomes its opposite.
      switch (physical) {
        case Dir::Up:    return Dir::Down;
        case Dir::Down:  return Dir::Up;
        case Dir::Left:  return Dir::Right;
        default:         return Dir::Left;
      }

    // THE TWO LANDSCAPES WERE EXCHANGED ON 2026-09-23, on device evidence.
    //
    // They started the other way round because the only clue in the codebase
    // was the old `|| btnRight` patch — every list screen read "Up OR Right
    // moves up", so the author's case had its right-hand button pointing up the
    // page. That is evidence about the person who wrote the patch, not about
    // how this device is held: Ardosia is used with the bottom edge of the case
    // to the RIGHT and the side keys above, which is the other landscape, and
    // on the panel the mapping came out exactly reversed.
    //
    // Both cases were exchanged rather than one, which keeps them mirrors of
    // each other (the tests check that) — and it means the same one-line fix
    // works whichever of the two orientation values is selected in Settings,
    // since reversing both reverses whichever one is in use.
    case Orientation::LANDSCAPE_CW:
      switch (physical) {
        case Dir::Left:  return Dir::Up;
        case Dir::Up:    return Dir::Right;
        case Dir::Right: return Dir::Down;
        default:         return Dir::Left;   // physical Down
      }

    case Orientation::LANDSCAPE_CCW:
      // The mirror: this is the one the old patch described.
      switch (physical) {
        case Dir::Right: return Dir::Up;
        case Dir::Down:  return Dir::Right;
        case Dir::Left:  return Dir::Down;
        default:         return Dir::Left;   // physical Up
      }
  }
  return physical;
}
