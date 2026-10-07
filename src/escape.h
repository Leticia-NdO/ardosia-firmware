#pragma once

#include <stdint.h>

// --- The generalised escape combo -------------------------------------------
//
// The vendored FreeInk hatch (lib/RecoveryBoot) escapes DOWNWARD ONLY: its
// switchToSlot0AndReboot() is hardcoded to OTA slot 0 and returns early when
// the running partition already IS slot 0 (RecoveryBoot.cpp:81-86).
//
// That leaves a real hole. Every Ardosia release artifact installs into ota_0
// (the merged image writes the app at 0x10000), so on a released install the
// combo's fallback does nothing at all: Ardosia is slot 0, so there is nowhere
// "down" to go, and the only remaining exit is /update.bin — which needs the
// user to already have a replacement image on a card.
//
// This closes it by escaping to THE OTHER SLOT, whichever one that is, and
// whatever it holds: a recovery hatch, the stock Xteink firmware, CrossPoint,
// or a previous Ardosia. Anything that boots is a better place to be than a
// firmware you cannot leave.
//
// Deliberately a pure predicate so it can be exercised on the host. The three
// inputs are all the decision needs, and the function is written to make the
// negative properties checkable: it must never switch without the combo, never
// switch into a slot with no app, and never switch twice in a row.
struct EscapeInputs {
  // The recovery combo was still held after the vendored check declined to act.
  bool comboHeld = false;

  // The other app slot begins with the ESP image magic (0xE9). Deliberately a
  // magic-byte test and not esp_image_verify(), matching the vendored hasApp():
  // esp_image_verify rejects the patched vendor images this device family ships,
  // which is the whole reason switchBootPartition() exists.
  bool otherHasApp = false;

  // This boot is the one right after we ourselves switched slots. Without this,
  // two firmwares that both implement this check would hand the device back and
  // forth for as long as the buttons are held: we switch to them, their own
  // combo check switches back to us, ours fires again. The flag lives in RTC
  // memory, which survives esp_restart() but not a cold boot — exactly the
  // lifetime a one-hop loop breaker wants.
  bool justSwitched = false;
};

// Should the firmware repoint otadata at the other slot and restart?
bool escapeShouldSwitch(const EscapeInputs& in);
