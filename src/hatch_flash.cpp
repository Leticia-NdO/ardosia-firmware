#include "hatch_flash.h"

#include <Arduino.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>

// The embedded payload. EMBED_FILES in src/CMakeLists.txt turns
// assets/hatch.bin into these two symbols.
extern const uint8_t kHatchBlobStart[] asm("_binary_hatch_bin_start");
extern const uint8_t kHatchBlobEnd[] asm("_binary_hatch_bin_end");

namespace {

constexpr size_t SEC = 4096;          // flash sector
constexpr size_t BLK = 64 * 1024;     // block-erase granularity
constexpr size_t CHUNK = 4096;        // write/verify chunk

size_t blobSize() {
  return static_cast<size_t>(kHatchBlobEnd - kHatchBlobStart);
}

}  // namespace

const char* hatchFlashResultName(const HatchFlashResult r) {
  switch (r) {
    case HatchFlashResult::OK:          return "OK";
    case HatchFlashResult::NoBlob:      return "NO_BLOB";
    case HatchFlashResult::NoPartition: return "NO_PARTITION";
    case HatchFlashResult::TooLarge:    return "TOO_LARGE";
    case HatchFlashResult::BadImage:    return "BAD_IMAGE";
    case HatchFlashResult::EraseFail:   return "ERASE_FAIL";
    case HatchFlashResult::WriteFail:   return "WRITE_FAIL";
    case HatchFlashResult::VerifyFail:  return "VERIFY_FAIL";
  }
  return "?";
}

HatchContext hatchReadContext() {
  HatchContext ctx;

  const size_t len = blobSize();
  ctx.blobPresent = len > 0;
  if (ctx.blobPresent) hatchParseImage(kHatchBlobStart, len, &ctx.blob);

  // Anti-rollback: ask whether the bootloader would accept this image's
  // secure_version rather than assuming it would. Checked, never patched —
  // patching upward can ratchet the eFuse permanently.
  ctx.secureVersionAccepted =
      ctx.blob.valid ? esp_efuse_check_secure_version(ctx.blob.secureVersion) : false;

  // "Unlocked" only means recoverable GIVEN a computer and a data cable, so this
  // changes the wording and the recommendation, never whether the row exists.
  ctx.deviceLocked = esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE);

  // The other slot: read enough of its head to parse a descriptor. A partition
  // that has never been written reads 0xFF and simply fails to parse.
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (other) {
    uint8_t head[kAppDescOff + 16 + 2 * kAppStrLen];
    if (esp_partition_read(other, 0, head, sizeof(head)) == ESP_OK) {
      ctx.otherSlotHasApp = hatchParseImage(head, sizeof(head), &ctx.otherSlot);
    }
  }
  return ctx;
}

bool hatchSlotHoldsEmbeddedHatch(const esp_partition_t* part) {
  if (!part) return false;

  const size_t len = blobSize();
  if (len == 0) return false;

  AppImageInfo blob;
  if (!hatchParseImage(kHatchBlobStart, len, &blob)) return false;

  uint8_t head[kAppDescOff + 16 + 2 * kAppStrLen];
  if (esp_partition_read(part, 0, head, sizeof(head)) != ESP_OK) return false;

  AppImageInfo there;
  if (!hatchParseImage(head, sizeof(head), &there)) return false;

  return hatchSameBuild(blob, there);
}

HatchFlashResult hatchFlashToOtherSlot(const HatchProgressCb onProgress, void* ctx) {
  const size_t total = blobSize();
  if (total == 0) return HatchFlashResult::NoBlob;

  AppImageInfo info;
  if (!hatchParseImage(kHatchBlobStart, total, &info) ||
      info.chipId != kChipIdEsp32C3) {
    return HatchFlashResult::BadImage;
  }

  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) return HatchFlashResult::NoPartition;
  if (total > dest->size) return HatchFlashResult::TooLarge;

  // Interleave erase and write so progress moves the whole way instead of
  // stalling on one up-front erase of the entire slot.
  size_t pos = 0;
  size_t erasedUpto = 0;
  while (pos < total) {
    if (pos >= erasedUpto) {
      size_t eraseLen = std::min<size_t>(BLK, dest->size - pos);
      eraseLen = (eraseLen + SEC - 1) & ~(SEC - 1);
      eraseLen = std::min<size_t>(eraseLen, dest->size - pos);
      if (esp_partition_erase_range(dest, pos, eraseLen) != ESP_OK) {
        return HatchFlashResult::EraseFail;
      }
      erasedUpto = pos + eraseLen;
    }

    const size_t want = std::min<size_t>(CHUNK, total - pos);
    if (esp_partition_write(dest, pos, kHatchBlobStart + pos, want) != ESP_OK) {
      return HatchFlashResult::WriteFail;
    }
    pos += want;
    if (onProgress) onProgress(pos, total, ctx);
    // Same yield the vendored loop takes: this runs on the Arduino loop task
    // with the NimBLE host alive beside it.
    delay(1);
  }

  // Read back. Not about transit corruption — the payload came inside an image
  // the bootloader already hash-verified — but about the flash having actually
  // taken it. A slot that reads back wrong must not be left looking installed,
  // because the whole promise is that Back + Up reaches something bootable.
  uint8_t buf[CHUNK];
  for (size_t off = 0; off < total; off += CHUNK) {
    const size_t want = std::min<size_t>(CHUNK, total - off);
    if (esp_partition_read(dest, off, buf, want) != ESP_OK) {
      return HatchFlashResult::VerifyFail;
    }
    if (memcmp(buf, kHatchBlobStart + off, want) != 0) {
      return HatchFlashResult::VerifyFail;
    }
  }

  // Deliberately NOT switchBootPartition(): we provisioned a recovery slot, we
  // did not ask to boot it. The device keeps booting Ardosia; Back + Up is what
  // reaches the hatch now (see src/escape.h).
  return HatchFlashResult::OK;
}
