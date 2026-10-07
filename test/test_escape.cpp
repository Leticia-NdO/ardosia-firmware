// The generalised escape combo (src/escape.cpp).
//
// The vendored FreeInk hatch escapes downward only — hardcoded to OTA slot 0,
// and a no-op when the running partition already IS slot 0. Every Ardosia
// release artifact installs into ota_0, so on a released install the combo's
// fallback does nothing and the device has no button-reachable way out. This
// predicate closes that: escape to the other slot, whichever it is.
//
// The weight here is carried by NEGATIVE properties. A false positive costs a
// reboot into another firmware, and on a USB-locked device the escape path is
// the one piece of behaviour that has no second chance — so what matters is
// what the predicate REFUSES to do, swept over every input combination rather
// than spot-checked.

#include <cstdio>

#include "escape.h"

static int failures = 0;

static void check(const bool ok, const char* what) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

// Every input combination. Three booleans is eight cases, so there is no reason
// to sample instead of enumerate.
struct Case { bool combo, other, just, want; };
static const Case kAll[] = {
    //  combo  otherHasApp  justSwitched  -> switch?
    {false, false, false, false},
    {false, false, true,  false},
    {false, true,  false, false},
    {false, true,  true,  false},
    {true,  false, false, false},
    {true,  false, true,  false},
    {true,  true,  true,  false},
    {true,  true,  false, true},   // the only yes
};

static EscapeInputs inputsOf(const Case& c) {
  EscapeInputs in;
  in.comboHeld = c.combo;
  in.otherHasApp = c.other;
  in.justSwitched = c.just;
  return in;
}

static void testTruthTable() {
  printf("the whole truth table, all eight cases\n");
  for (const Case& c : kAll) {
    char what[96];
    snprintf(what, sizeof(what), "combo=%d other=%d just=%d -> %s",
             c.combo, c.other, c.just, c.want ? "switch" : "stay");
    check(escapeShouldSwitch(inputsOf(c)) == c.want, what);
  }
}

static void testExactlyOneWayToSwitch() {
  printf("switching has exactly one shape\n");

  int yes = 0;
  for (const Case& c : kAll) {
    if (escapeShouldSwitch(inputsOf(c))) yes++;
  }
  check(yes == 1, "out of eight input combinations, exactly one switches");

  // Named individually because each is a different way to brick someone's day,
  // and a reader of this file should see them stated rather than inferred from
  // the table above.
  EscapeInputs noCombo;
  noCombo.comboHeld = false;
  noCombo.otherHasApp = true;
  noCombo.justSwitched = false;
  check(!escapeShouldSwitch(noCombo),
        "never switches unasked — no combo, no reboot, however good the target");

  EscapeInputs blankTarget;
  blankTarget.comboHeld = true;
  blankTarget.otherHasApp = false;
  blankTarget.justSwitched = false;
  check(!escapeShouldSwitch(blankTarget),
        "never switches into a slot with no app — that would strand the user");

  EscapeInputs pingPong;
  pingPong.comboHeld = true;
  pingPong.otherHasApp = true;
  pingPong.justSwitched = true;
  check(!escapeShouldSwitch(pingPong),
        "never switches twice in a row — breaks the two-firmware ping-pong");
}

static void testDefaultsAreTheSafeAnswer() {
  printf("a default-constructed input does nothing\n");

  // This matters because the caller fills the struct field by field. If a field
  // is ever added and one call site forgets it, the uninitialised default must
  // fall on the side of not rebooting.
  const EscapeInputs fresh;
  check(!escapeShouldSwitch(fresh), "the default-constructed inputs do not switch");
}

static void testHeldComboIsNecessaryNotSufficient() {
  printf("the combo alone is never enough\n");

  // The vendored hatch's own rule is the same shape: holding the combo is the
  // request, not the authorisation. Both other conditions are about whether
  // honouring it would help.
  for (const Case& c : kAll) {
    if (!c.combo) continue;
    if (c.other && !c.just) continue;   // the legitimate yes
    char what[96];
    snprintf(what, sizeof(what),
             "combo held but other=%d just=%d still stays", c.other, c.just);
    check(!escapeShouldSwitch(inputsOf(c)), what);
  }
}

int main() {
  printf("\n=== generalised escape combo ===\n\n");
  testTruthTable();
  printf("\n");
  testExactlyOneWayToSwitch();
  printf("\n");
  testDefaultsAreTheSafeAnswer();
  printf("\n");
  testHeldComboIsNecessaryNotSufficient();
  printf("\n");

  if (failures) {
    printf("FAILURES: %d\n", failures);
    return 1;
  }
  printf("escape combo: every check passed\n");
  return 0;
}
