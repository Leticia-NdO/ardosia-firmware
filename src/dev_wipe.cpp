#include "dev_wipe.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "config.h"

namespace {
constexpr char kSentinel[] = "/ARDOSIA-WIPE-OTHER-SLOT";
constexpr char kSentinelDone[] = "/ARDOSIA-WIPE-OTHER-SLOT.done";
constexpr size_t kSector = 4096;
}  // namespace

DevWipeOutcome devWipeOtherSlotIfArmed() {
  if (!SdMan.exists(kSentinel)) return DevWipeOutcome::NotArmed;

  // Disarm FIRST, act second. If the erase were to hang or the device reset
  // mid-way, a sentinel still on the card would wipe again on the next boot,
  // and again — a boot loop that eats the slot every time. Renaming before
  // touching flash makes that impossible. The cost of this order is that a
  // failed erase does not retry, which is the right trade: it is a test tool,
  // and the user can put the file back.
  SdMan.remove(kSentinelDone);
  if (!SdMan.rename(kSentinel, kSentinelDone)) {
    // Could not disarm — do nothing rather than risk the loop above.
    return DevWipeOutcome::CouldNotDisarm;
  }

  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!other) return DevWipeOutcome::NoPartition;

  // Cannot be the running slot: esp_ota_get_next_update_partition() excludes it
  // by definition. That is what makes this safe to do at all.
  if (esp_partition_erase_range(other, 0, kSector) != ESP_OK) {
    return DevWipeOutcome::EraseFailed;
  }

  DBG_PRINTF("[DEV] wiped header of %s @0x%x\n", other->label,
             static_cast<unsigned>(other->address));
  return DevWipeOutcome::Done;
}
