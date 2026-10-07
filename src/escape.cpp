#include "escape.h"

bool escapeShouldSwitch(const EscapeInputs& in) {
  // Order matters only for readability; all three are necessary.
  if (!in.comboHeld) return false;      // never act unasked
  if (!in.otherHasApp) return false;    // never strand on a blank partition
  if (in.justSwitched) return false;    // never ping-pong
  return true;
}
