#pragma once

#include <stdint.h>

// --- Wipe the other OTA slot — TEMPORARY, for testing a clean install --------
//
// Makes the OTHER app slot read as empty, so the "fresh install" state can be
// reproduced on a device that already has firmware in both slots. It exists to
// test the Settings > System > Recovery flow (src/hatch.h) from the state a new
// user actually starts in, which is otherwise unreachable without USB.
//
// Armed by a FILE ON THE SD CARD, not by a menu row:
//
//   create an empty /ARDOSIA-WIPE-OTHER-SLOT on the card, boot, and it runs
//   once; the file is renamed to .done, so it cannot run twice
//
// Why a sentinel file and not a Settings row:
//   - it takes a deliberate physical act, so it cannot be hit by accident on a
//     device whose only recovery route might be the slot it erases;
//   - it is self-limiting by construction — the same property dropLegacyDiagKeys()
//     has, and the reason that one was safe to ship;
//   - no UI, no SETTINGS_COUNT change, nothing to find and unpick later. Removing
//     this is deleting two files and one call.
//
// WHY THIS IS RECOVERABLE, which is the only reason it is acceptable at all:
// esp_ota_get_next_update_partition() is by definition NOT the running slot, so
// this can never erase the firmware executing it. Afterwards the device still
// boots this Ardosia, and Ardosia carries the recovery firmware inside its own
// image (assets/hatch.bin) — so Settings > System > Recovery puts a hatch back
// with no SD file and no USB. The wipe and the reinstall are inverses, which is
// what makes this a test rather than a gamble.
//
// It erases ONE SECTOR, not the whole 6.25 MB slot. Every check in this
// codebase that asks "is there an app there" reads the header: the 0xE9 magic
// (recovery::hasApp, escapePartitionHasApp) or the app descriptor at 0x20
// (detectOtaApps, hatchReadContext) — all inside the first 4 KiB. So one sector
// is indistinguishable from empty to all of them, costs one erase cycle instead
// of 1600, and takes milliseconds. The slot is not *blank* afterwards, only
// *unbootable and undetectable*, which is exactly what is being simulated.
//
// DELETE BEFORE RELEASE. src/dev_wipe.* and the devWipeOtherSlotIfArmed() call
// in setup().

enum class DevWipeOutcome : uint8_t {
  NotArmed,      // no sentinel file — the normal path, every boot
  Done,
  NoCard,        // sentinel unreadable / card absent
  CouldNotDisarm,// refused: the sentinel could not be renamed, so it would repeat
  NoPartition,
  EraseFailed,
};

// Checks for the sentinel and, if present, disarms it and erases the other
// slot's header. Safe and almost free to call unconditionally.
DevWipeOutcome devWipeOtherSlotIfArmed();
