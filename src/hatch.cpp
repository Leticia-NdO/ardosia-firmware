#include "hatch.h"

#include <stdio.h>
#include <string.h>

namespace {

uint16_t rdU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

uint32_t rdU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Copy a fixed-width image string into a NUL-terminated buffer without
// trusting it to be terminated in the image.
void copyField(const uint8_t* src, char* dst) {
  memcpy(dst, src, kAppStrLen);
  dst[kAppStrLen] = '\0';
}

}  // namespace

bool hatchParseImage(const uint8_t* data, const size_t len, AppImageInfo* out) {
  if (!out) return false;
  *out = AppImageInfo{};
  if (!data) return false;

  // The descriptor ends at 0x20 + 4 + 4 + 8 + 32 + 32; require at least that
  // much rather than reading past a short prefix.
  const size_t need = kAppDescOff + 16 + 2 * kAppStrLen;
  if (len < need) return false;

  if (data[0] != kEspImageMagic) return false;
  if (rdU32(data + kAppDescOff) != kAppDescMagic) return false;

  out->chipId = rdU16(data + 12);
  out->secureVersion = rdU32(data + kAppDescOff + 4);
  // esp_app_desc_t: magic(4) secure_version(4) reserv1(8) version[32] project_name[32]
  copyField(data + kAppDescOff + 16, out->version);
  copyField(data + kAppDescOff + 16 + kAppStrLen, out->projectName);
  out->valid = true;
  return true;
}

bool hatchSameBuild(const AppImageInfo& a, const AppImageInfo& b) {
  if (!a.valid || !b.valid) return false;
  return memcmp(a.version, b.version, kAppStrLen) == 0 &&
         memcmp(a.projectName, b.projectName, kAppStrLen) == 0;
}

HatchOffer hatchOfferFor(const HatchContext& ctx) {
  // Order is the point here. Each check answers "is there any sense in offering
  // this", and the cheapest-to-be-wrong ones come first.
  if (!ctx.blobPresent || !ctx.blob.valid) return HatchOffer::Unavailable;
  if (ctx.blob.chipId != kChipIdEsp32C3) return HatchOffer::WrongChip;

  // Anti-rollback: on a unit whose eFuse minimum exceeds the blob's
  // secure_version the bootloader refuses the image. Writing it anyway is not
  // destructive — the bootloader falls back to this slot — but it would claim a
  // recovery route that silently does not work, which is worse than saying no.
  //
  // Checked, never patched. Patching secure_version upward can ratchet the
  // eFuse permanently (escape-hatch/tools/patch_secure_version.py says so in as
  // many words), and this fork does not do irreversible.
  if (!ctx.secureVersionAccepted) return HatchOffer::RefusedRollback;

  if (ctx.otherSlotHasApp && hatchSameBuild(ctx.blob, ctx.otherSlot)) {
    return HatchOffer::AlreadyPresent;
  }
  return ctx.otherSlotHasApp ? HatchOffer::Replace : HatchOffer::Install;
}

HatchUrgency hatchUrgencyFor(const bool deviceLocked) {
  return deviceLocked ? HatchUrgency::Recommended : HatchUrgency::Optional;
}

bool hatchOfferIsActionable(const HatchOffer offer) {
  return offer == HatchOffer::Install || offer == HatchOffer::Replace;
}

void hatchOfferText(const HatchContext& ctx, const HatchOffer offer, char* out,
                    const size_t n) {
  if (!out || n == 0) return;
  switch (offer) {
    case HatchOffer::Unavailable:
      snprintf(out, n, "Not in this build");
      return;
    case HatchOffer::WrongChip:
      snprintf(out, n, "Wrong chip");
      return;
    case HatchOffer::RefusedRollback:
      snprintf(out, n, "Blocked (anti-rollback)");
      return;
    case HatchOffer::AlreadyPresent:
      snprintf(out, n, "Installed");
      return;
    case HatchOffer::Install:
      snprintf(out, n, "Install");
      return;
    case HatchOffer::Replace: {
      // Name what would be lost. The row is the last place this is visible
      // before the confirm dialog asks, and "Replace" alone would hide that
      // something is being destroyed.
      char victim[kAppStrLen + 1];
      memcpy(victim, ctx.otherSlot.projectName, kAppStrLen);
      victim[kAppStrLen] = '\0';
      // Trim the padding the field carries so the row does not read "Replace
      // crosspoint            ".
      for (int i = static_cast<int>(strlen(victim)) - 1; i >= 0; --i) {
        if (victim[i] == ' ' || victim[i] == '\0') victim[i] = '\0';
        else break;
      }
      if (victim[0] == '\0') snprintf(out, n, "Replace slot");
      else                   snprintf(out, n, "Replace %s", victim);
      return;
    }
  }
  snprintf(out, n, "-");
}
