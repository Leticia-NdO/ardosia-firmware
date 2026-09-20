#include "sleep_layout.h"

#include <algorithm>

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

int sleepScreenNextIndex(const SleepScreenMode mode, const int count,
                         const bool haveHistory, const uint32_t last, const uint32_t roll) {
  if (count <= 1) return 0;
  const auto n = static_cast<uint32_t>(count);

  if (mode == SleepScreenMode::SHUFFLE) {
    uint32_t next = roll % n;
    // Never the same wallpaper twice running — with three or four images a
    // plain random pick repeats often enough to look broken.
    if (haveHistory && next == last % n) next = (next + 1) % n;
    return static_cast<int>(next);
  }

  return static_cast<int>(haveHistory ? (last + 1) % n : 0);
}

// Ordered 4x4 Bayer matrix, values 0..15. Ordered rather than error-diffused on
// purpose: it needs no per-row state, so the draw loop stays a single pass over
// the SD card with no buffers, and the regular texture survives the
// nearest-neighbour scaling that drawing a wallpaper involves.
static const uint8_t BAYER_4X4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

int sleepScreenLevelDensity(const uint8_t level, const SleepBrightness brightness) {
  // Rows are levels 0..3 (black, dark, light, white); values are dot density
  // out of 16. Level 3 is always 0 and level 0 always 16: brightening should
  // lift the mid tones, not punch holes in the blacks or grey out the paper.
  static const int DENSITY[3][4] = {
      {16, 12, 8, 0},   // NORMAL
      {16,  9, 4, 0},   // LIGHT
      {16,  6, 2, 0},   // LIGHTER
  };
  const int row = static_cast<int>(brightness);
  if (row < 0 || row > 2 || level > 3) return 0;
  return DENSITY[row][level];
}

bool sleepScreenPixelIsBlack(const uint8_t level, const int x, const int y,
                             const SleepBrightness brightness) {
  const int density = sleepScreenLevelDensity(level, brightness);
  if (density <= 0) return false;
  if (density >= 16) return true;
  // Negative coordinates never reach here, but keep the modulo well-defined.
  const int bx = ((x % 4) + 4) % 4;
  const int by = ((y % 4) + 4) % 4;
  return BAYER_4X4[by * 4 + bx] < density;
}
