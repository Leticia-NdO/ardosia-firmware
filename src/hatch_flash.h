#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_partition.h>

#include "hatch.h"

// --- Embedded recovery firmware: the flash-write half ------------------------
//
// Separated from hatch.h so that the decision logic stays free of Arduino and
// ESP-IDF and can be exercised on the host. Same split, same reason, as
// deadkeys.* against input_handler.
//
// Why not reuse freeink::firmware::flashFromSdPath(): it takes a PATH, and the
// payload here is a pointer into our own image. It also ends by calling
// switchBootPartition(dest), which is right for "flash a firmware and boot it"
// and wrong for this — we are provisioning a recovery slot and must keep
// booting Ardosia. Rather than call it and undo the otadata change afterwards,
// this writes the bytes and touches otadata not at all.
//
// lib/RecoveryBoot therefore stays byte-for-byte against upstream. The erase /
// write shape below is modelled on its flashFromSdPath() loop on purpose: 4 KiB
// writes interleaved with 64 KiB erases, so a progress bar advances smoothly
// instead of stalling for seconds on one up-front erase.

enum class HatchFlashResult : uint8_t {
  OK,
  NoBlob,
  NoPartition,
  TooLarge,
  BadImage,      // the blob does not parse / is not for this chip
  EraseFail,
  WriteFail,
  VerifyFail,    // read-back disagreed with what we wrote
};

const char* hatchFlashResultName(HatchFlashResult r);

// written/total in bytes, called after every chunk.
using HatchProgressCb = void (*)(size_t written, size_t total, void* ctx);

// Write the embedded blob into the OTHER OTA slot. Does not reboot and does not
// change the boot partition: on return the device still boots this firmware.
//
// Verifies by reading back after the write. The payload cannot have been
// corrupted in transit — it rode inside an image the bootloader hash-verified,
// and test/test_hatch.cpp checks its integrity at build time — so the read-back
// is about the FLASH accepting it, which is a different failure and a real one.
HatchFlashResult hatchFlashToOtherSlot(HatchProgressCb onProgress = nullptr,
                                       void* ctx = nullptr);

// Everything the Settings row needs to decide what to show. Reads the eFuses,
// the embedded blob and the other slot's descriptor.
HatchContext hatchReadContext();

// Does this partition hold the same recovery build this firmware carries?
//
// Exact, and needs no NVS: it compares the partition's own app descriptor
// against the embedded blob's. That is what lets the main menu label the slot
// "Recovery" even for a hatch that was put there before this firmware existed
// — and without it the label would fall back to the blob's project_name, which
// the Arduino core sets to "arduino-lib-builder" and which identifies nothing.
//
// Declared here rather than in hatch.h because it reads flash. `part` may be
// null, which answers false.
bool hatchSlotHoldsEmbeddedHatch(const esp_partition_t* part);
