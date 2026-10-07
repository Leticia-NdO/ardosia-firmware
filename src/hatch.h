#pragma once

#include <stddef.h>
#include <stdint.h>

// --- Embedded recovery firmware: the decision half ---------------------------
//
// Ardosia carries the Escape Hatch recovery firmware inside its own image
// (assets/hatch.bin, ~486 KB of a 6.25 MB slot) and can write it into the unused
// OTA slot. The point is that a USB-locked device ends up with a real recovery
// firmware it can always reach with Back + Up, without the user having to
// install anything first.
//
// Everything here is pure: parsing an ESP32 app image's descriptor, and
// deciding what the Settings row should offer. The flash write lives in
// src/hatch_flash.h so that this half can be exercised on the host — the same
// split, and for the same reason, as deadkeys.* against input_handler.
//
// See ../SPEC-firmware-distribution.md §10 for why embedded rather than
// downloaded, and §10.4/§10.5 for the two risks the logic below exists to
// handle.

// Offsets into an ESP32 app image, from esp_image_header_t + esp_app_desc_t.
// Confirmed against escape-hatch/tools/patch_secure_version.py, which rewrites
// these same fields, and against the images themselves.
static constexpr size_t kImgHeaderSize = 24;   // esp_image_header_t
static constexpr size_t kImgSegHeader = 8;     // load_addr + data_len
static constexpr size_t kAppDescOff = kImgHeaderSize + kImgSegHeader;  // 0x20
static constexpr uint8_t kEspImageMagic = 0xE9;
static constexpr uint32_t kAppDescMagic = 0xABCD5432;
static constexpr uint16_t kChipIdEsp32C3 = 5;
static constexpr size_t kAppStrLen = 32;       // version[32], project_name[32]

// What an app image says about itself. The two strings are kept as fixed-width
// fields rather than trimmed: the firmware compares them byte for byte against
// another partition's, and trimming invites two spellings of "equal".
struct AppImageInfo {
  bool valid = false;
  uint16_t chipId = 0;
  uint32_t secureVersion = 0;
  char version[kAppStrLen + 1] = {0};
  char projectName[kAppStrLen + 1] = {0};
};

// Parse the header and app descriptor out of the first bytes of an image.
// `len` may be the whole image or just a prefix; anything shorter than the
// descriptor is rejected rather than read past. Returns in.valid.
bool hatchParseImage(const uint8_t* data, size_t len, AppImageInfo* out);

// True when two descriptors name the same build. Used for idempotence: writing
// 486 KB again every time someone opens Settings is flash wear for nothing.
//
// Compares version AND project name, and nothing else. Note that the hatch's
// project name is "arduino-lib-builder" — the Arduino core sets it, not the
// project — so the name alone identifies far too much; the version string is
// the real discriminator (a git hash, e.g. "8cabf2c"). Both are compared
// because neither alone is a promise.
bool hatchSameBuild(const AppImageInfo& a, const AppImageInfo& b);

// What the Settings row can offer right now.
enum class HatchOffer : uint8_t {
  Unavailable,      // no usable blob embedded — the row says so and does nothing
  WrongChip,        // the blob is not for this MCU family
  RefusedRollback,  // the bootloader would not accept it (anti-rollback eFuse)
  AlreadyPresent,   // the other slot already holds this exact build
  Install,          // the other slot is empty: a plain install
  Replace,          // the other slot holds something else, which will be LOST
};

// How strongly to push it. Derived from the device alone, and deliberately
// kept out of HatchOffer: see hatchOfferFor().
enum class HatchUrgency : uint8_t {
  Recommended,  // USB-locked: the boot combo is the only way out
  Optional,     // serial download still enabled: USB is already a way out
};

struct HatchContext {
  AppImageInfo blob;        // the embedded payload
  bool blobPresent = false; // non-empty EMBED_FILES region
  bool secureVersionAccepted = false;  // esp_efuse_check_secure_version(blob)
  bool otherSlotHasApp = false;
  AppImageInfo otherSlot;   // only meaningful when otherSlotHasApp
  bool deviceLocked = false;  // ESP_EFUSE_DIS_DOWNLOAD_MODE
};

// The offer does NOT depend on deviceLocked — that is the whole point, and the
// tests assert it over every context.
//
// The fork's convention is "não filtrar informação da UI de forma irreversível
// em aparelho sem USB", and bug #5 settled this exact shape with "marca, nunca
// esconde". Lock state is also a weaker signal than it looks: "unlocked" means
// recoverable GIVEN a computer and a data cable, and the X3 on the author's desk
// has serial download enabled and a power-only pogo cable — unlocked, and in
// practice unflashable. So lock state changes the wording and the recommendation
// (HatchUrgency), never whether the row exists.
HatchOffer hatchOfferFor(const HatchContext& ctx);
HatchUrgency hatchUrgencyFor(bool deviceLocked);

// True when choosing the row should actually write anything.
bool hatchOfferIsActionable(HatchOffer offer);

// Short text for the row's value column, e.g. "Install" / "Replace crosspoint".
// `out` gets a NUL-terminated string; `n` should be at least 40.
void hatchOfferText(const HatchContext& ctx, HatchOffer offer, char* out, size_t n);
