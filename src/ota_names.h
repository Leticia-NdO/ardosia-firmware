#pragma once

#include <stddef.h>

// --- What to call another OTA slot in the main menu --------------------------
//
// The bug this exists to fix: the nickname in NVS was keyed by SLOT NUMBER and
// nothing tied it to what was actually in the slot. Overwrite the slot with a
// completely different firmware and the old name stayed — a device whose ota_0
// had been CrossInk still offered "CrossInk" in the menu after the Escape Hatch
// was written over it. Pressing it correctly booted the hatch; only the label
// lied.
//
// Keeping a hand-set name across an UPDATE of the same app is deliberate and
// worth keeping (renameOtaApp's comment says so). Keeping it across a
// REPLACEMENT by a different app is the defect. The fix is to remember which
// project the nickname was set for, and drop it when the project changes: the
// nickname belongs to an app in a slot, not to the slot.
//
// Pure so it can be tested on the host. All four fields are read straight out
// of NVS and the partition's esp_app_desc_t by the caller.
struct OtaNameInputs {
  // NVS ota_<slot> — a name set by the app itself at boot, or by the user with
  // Ctrl+N on the menu row. Empty when none.
  const char* nickname = "";

  // NVS otap_<slot> — the project_name the nickname was recorded against.
  // Empty for a nickname written before this was tracked, which cannot be
  // verified and is therefore treated as stale: the menu recomputes from the
  // descriptor instead. That costs a one-time loss of hand-set names and makes
  // the display self-healing, which is the better trade on a device where the
  // wrong label points at the wrong firmware.
  const char* nicknameProject = "";

  // esp_app_desc_t::project_name of whatever is in the slot RIGHT NOW. Empty
  // when the slot holds no parseable app.
  const char* slotProject = "";

  // The slot's descriptor matches the recovery firmware embedded in this build
  // (see hatch.h). Exact, needs no NVS, and identifies a hatch installed by any
  // means — including one put there before this firmware existed.
  bool slotIsEmbeddedHatch = false;

  int slot = 0;
};

// Writes a NUL-terminated label. `n` should be at least 32 (OtaAppEntry::name).
void otaDisplayName(const OtaNameInputs& in, char* out, size_t n);

// Trim the trailing spaces and NULs that esp_app_desc_t's fixed-width char
// arrays carry, so a label does not read "crosspoint          ".
void otaTrimField(const char* src, char* out, size_t n);
