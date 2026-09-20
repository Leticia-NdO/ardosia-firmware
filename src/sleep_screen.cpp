#include "sleep_screen.h"

#include "sleep_layout.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <esp_attr.h>
#include <esp_random.h>

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// Custom sleep screens.
//
// Modelled on crosspoint-reader's SleepActivity (MIT), which this firmware
// already shares its Bitmap/GfxRenderer code with — lib/GfxRenderer here is the
// same vendored source. Kept deliberately smaller: crosspoint also does PNG,
// alpha overlays, book covers and quick-resume, none of which a writing device
// needs.
//
// Wallpapers are BMPs. Locations, in the order they are tried:
//   /sleep/      a directory of images  (the slideshow)
//   /.sleep/     same, hidden — crosspoint's own path, so an SD card set up for
//                a CrossPoint device works here unchanged
//   /sleep.bmp   a single image
// ---------------------------------------------------------------------------

namespace {

const char* const SLEEP_DIRS[] = {"/sleep", "/.sleep"};
constexpr char SLEEP_SINGLE_FILE[] = "/sleep.bmp";

// Where the slideshow is. RTC memory, not NVS: it survives deep sleep (which is
// the only transition that matters here) and costs no flash wear, and flash
// wear on a per-sleep path is exactly the debt this repo has already paid once.
//
// It does NOT survive a cold power-on, and the magic guards against reading
// garbage on the first boot after a flash. Degraded behaviour is "the slideshow
// restarts at the first image", which is visible and harmless.
constexpr uint32_t SLEEP_RTC_MAGIC = 0x534C5031;  // "SLP1"
RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR uint32_t rtcLastIndex = 0;

constexpr size_t MAX_SLEEP_NAME = 64;

bool hasBmpExtension(const char* name) {
  const size_t n = strlen(name);
  return n > 4 && strcasecmp(name + n - 4, ".bmp") == 0;
}

// A file counts only if its BMP header actually parses. Validating up front is
// what stops a truncated or non-BMP file from producing a blank sleep screen
// with no way to tell why.
bool isDrawableBmp(FsFile& file) {
  Bitmap probe(file);
  const bool ok = probe.parseHeaders() == BmpReaderError::Ok;
  file.rewind();
  return ok;
}

// Walk `dir` to the next drawable BMP and copy its name into `name`.
//
// Returns the NAME, not the open file: FsFile is neither copyable nor movable
// out of here, and the caller wants a path it can reopen anyway.
bool nextDrawableName(FsFile& dir, char* name, const size_t nameLen) {
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) { file.close(); continue; }
    file.getName(name, nameLen);
    const bool candidate = name[0] != '\0' && name[0] != '.' && hasBmpExtension(name);
    const bool drawable = candidate && isDrawableBmp(file);
    file.close();
    if (drawable) return true;
  }
  return false;
}

// Two passes over the directory instead of building a list of names: the file
// count is unbounded and this chip has 320KB of RAM in total.
int countDrawable(const char* dirPath) {
  auto dir = SdMan.open(dirPath);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return 0; }

  char name[MAX_SLEEP_NAME];
  int count = 0;
  dir.rewindDirectory();
  while (nextDrawableName(dir, name, sizeof(name))) {
    if (++count >= 1000) break;   // sanity ceiling
  }
  dir.close();
  return count;
}

// Build "<dirPath>/<name of the index-th drawable BMP>" into `outPath`.
bool pathOfDrawableAt(const char* dirPath, const int index, char* outPath, const size_t outLen) {
  auto dir = SdMan.open(dirPath);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return false; }

  char name[MAX_SLEEP_NAME];
  dir.rewindDirectory();
  for (int i = 0; i <= index; i++) {
    if (!nextDrawableName(dir, name, sizeof(name))) { dir.close(); return false; }
  }
  dir.close();
  snprintf(outPath, outLen, "%s/%s", dirPath, name);
  return true;
}

// Pick which image this sleep shows, and remember it for the next one.
// The decision itself lives in sleep_layout.cpp so it can be tested on a host;
// this wrapper only owns the RTC state around it.
int chooseIndex(const SleepScreenMode mode, const int count) {
  const bool haveHistory = (rtcMagic == SLEEP_RTC_MAGIC);
  const int next = sleepScreenNextIndex(mode, count, haveHistory, rtcLastIndex, esp_random());
  rtcLastIndex = static_cast<uint32_t>(next);
  rtcMagic = SLEEP_RTC_MAGIC;
  return next;
}

// Draw the wallpaper, halftoned.
//
// This does NOT use GfxRenderer::drawBitmap(). That function is fine for icons
// but in BW mode it paints every level below pure white as solid black, and the
// quantiser's white threshold is 140 of 255 — so a photograph comes out as a
// slab of black. See sleep_layout.h.
//
// Instead the 2-bit rows come straight from Bitmap::readNextRow() (public API,
// no change to the vendored library) and each level is rendered as a dot
// density. One pass over the file, two small row buffers, no frame of state.
bool drawHalftoned(GfxRenderer& renderer, const Bitmap& bitmap, const SleepBrightness brightness) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const SleepPlacement place = sleepScreenFit(bitmap.getWidth(), bitmap.getHeight(), screenW, screenH);
  if (place.w <= 0 || place.h <= 0) return false;

  const int srcW = bitmap.getWidth();
  const int srcH = bitmap.getHeight();
  const float scale = static_cast<float>(place.w) / static_cast<float>(srcW);

  const int outputRowSize = (srcW + 3) / 4;
  auto* outputRow = static_cast<uint8_t*>(malloc(static_cast<size_t>(outputRowSize)));
  auto* rowBytes = static_cast<uint8_t*>(malloc(static_cast<size_t>(bitmap.getRowBytes())));
  if (!outputRow || !rowBytes) {
    free(outputRow);
    free(rowBytes);
    return false;
  }

  bool ok = true;
  int prevScreenY = INT32_MIN;
  for (int srcY = 0; srcY < srcH; srcY++) {
    // Every row has to be read, in order, even when it will not be drawn:
    // readNextRow() walks the file sequentially and skipping a call would
    // desynchronise every row after it.
    if (bitmap.readNextRow(outputRow, rowBytes) != BmpReaderError::Ok) {
      // A short read ends the draw rather than skipping ahead; whatever is
      // already on screen still shows.
      ok = (srcY > 0);
      break;
    }

    // A BMP is stored bottom-up unless its height is negative.
    const int imageY = bitmap.isTopDown() ? srcY : (srcH - 1 - srcY);
    const int screenY = place.y + static_cast<int>(static_cast<float>(imageY) * scale);
    if (screenY < 0 || screenY >= screenH) continue;

    // Scaling down maps several source rows onto one screen row. Drawing all of
    // them is pure waste, and on an 80MHz core a 12-megapixel wallpaper would
    // make "hold power to sleep" take visibly long.
    if (screenY == prevScreenY) continue;
    prevScreenY = screenY;

    int prevScreenX = INT32_MIN;
    for (int srcX = 0; srcX < srcW; srcX++) {
      const int screenX = place.x + static_cast<int>(static_cast<float>(srcX) * scale);
      if (screenX < 0 || screenX >= screenW) continue;
      if (screenX == prevScreenX) continue;   // same reason, along the row
      prevScreenX = screenX;

      // Two bits per pixel, packed high-to-low within each byte.
      const uint8_t level = (outputRow[srcX / 4] >> (6 - ((srcX * 2) % 8))) & 0x3;

      // Paint white explicitly as well as black: when scaling down, several
      // source pixels land on one screen pixel, and only painting the black
      // ones would let any dark pixel in the group win and darken the image.
      renderer.drawPixel(screenX, screenY,
                         sleepScreenPixelIsBlack(level, screenX, screenY, brightness));
    }
  }

  free(outputRow);
  free(rowBytes);
  return ok;
}

}  // namespace

bool sleepScreenDrawImage(GfxRenderer& renderer, const SleepScreenMode mode,
                          const SleepBrightness brightness) {
  if (mode == SleepScreenMode::TEXT) return false;

  char path[MAX_SLEEP_NAME + 16];
  path[0] = '\0';

  for (const char* candidate : SLEEP_DIRS) {
    const int count = countDrawable(candidate);
    if (count <= 0) continue;
    if (pathOfDrawableAt(candidate, chooseIndex(mode, count), path, sizeof(path))) break;
    path[0] = '\0';
  }

  if (path[0] == '\0') {
    if (!SdMan.exists(SLEEP_SINGLE_FILE)) return false;
    snprintf(path, sizeof(path), "%s", SLEEP_SINGLE_FILE);
  }

  auto file = SdMan.open(path);
  if (!file) return false;

  // dithering=false: the halftone is ours (sleep_layout.cpp). The library's own
  // ditherer quantises to four levels, which BW rendering would collapse again.
  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    return false;
  }

  renderer.clearScreen();
  const bool drawn = drawHalftoned(renderer, bitmap, brightness);
  file.close();
  SdMan.sleep();
  return drawn;
}
