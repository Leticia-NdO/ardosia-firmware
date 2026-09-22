#include "sleep_layout.h"

#include <algorithm>
#include <cstring>

SleepPlacement sleepScreenFit(const int bitmapW, const int bitmapH,
                              const int screenW, const int screenH) {
  SleepPlacement p{0, 0, 0, 0};
  if (bitmapW <= 0 || bitmapH <= 0 || screenW <= 0 || screenH <= 0) return p;

  float scale = 1.0f;
  if (bitmapW > screenW) scale = static_cast<float>(screenW) / static_cast<float>(bitmapW);
  if (bitmapH > screenH) {
    scale = std::min(scale, static_cast<float>(screenH) / static_cast<float>(bitmapH));
  }

  p.w = static_cast<int>(static_cast<float>(bitmapW) * scale);
  p.h = static_cast<int>(static_cast<float>(bitmapH) * scale);
  p.x = (screenW - p.w) / 2;
  p.y = (screenH - p.h) / 2;
  return p;
}

namespace {

uint16_t lastShown(const SleepRecent& recent) {
  if (recent.fill == 0 || recent.pos >= SLEEP_RECENT_CAP) return 0;
  const uint8_t slot =
      static_cast<uint8_t>((recent.pos + SLEEP_RECENT_CAP - 1) % SLEEP_RECENT_CAP);
  return recent.indices[slot];
}

}  // namespace

int sleepScreenNextIndex(const SleepScreenMode mode, const int count,
                         const bool haveHistory, const SleepRecent& recent, const uint32_t roll) {
  if (count <= 1) return 0;
  const auto n = static_cast<uint32_t>(count);

  if (mode != SleepScreenMode::SHUFFLE) {
    if (!haveHistory || recent.fill == 0) return 0;
    return static_cast<int>((lastShown(recent) + 1) % n);
  }

  // Same selection as CrossInk's ImageFolderIndex::chooseIndex: uniform among
  // images outside the recent window. The window stops at count-1 so a folder
  // of three still has one image that is not recent — otherwise, once the
  // window covers the folder, the fallback random can repeat the last picture.
  uint16_t blocked[SLEEP_RECENT_CAP];
  int blockedCount = 0;
  const int window = std::min({static_cast<int>(recent.fill), SLEEP_RECENT_CAP, count - 1});
  if (haveHistory) {
    for (int i = 0; i < window; i++) {
      const uint8_t slot = static_cast<uint8_t>(
          (recent.pos + SLEEP_RECENT_CAP - 1 - i) % SLEEP_RECENT_CAP);
      const uint16_t index = recent.indices[slot];
      if (index >= n) continue;
      bool seen = false;
      for (int j = 0; j < blockedCount; j++) {
        if (blocked[j] == index) seen = true;
      }
      if (!seen) blocked[blockedCount++] = index;
    }
    std::sort(blocked, blocked + blockedCount);
  }

  const int freeCount = count - blockedCount;
  uint16_t rank = static_cast<uint16_t>(freeCount > 0 ? roll % static_cast<uint32_t>(freeCount) : 0);
  for (int i = 0; i < blockedCount; i++) {
    if (blocked[i] <= rank) {
      rank++;
    } else {
      break;
    }
  }
  if (rank >= n) rank = static_cast<uint16_t>(n - 1);
  return static_cast<int>(rank);
}

void sleepScreenRemember(SleepRecent& recent, const uint16_t index) {
  if (recent.pos >= SLEEP_RECENT_CAP) recent.pos = 0;
  recent.indices[recent.pos] = index;
  recent.pos = static_cast<uint8_t>((recent.pos + 1) % SLEEP_RECENT_CAP);
  if (recent.fill < SLEEP_RECENT_CAP) recent.fill++;
}
