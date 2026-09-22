#include "sleep_screen.h"

#include "sleep_layout.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <esp_attr.h>
#include <esp_random.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

// Custom sleep screens, following CrossInk's SleepActivity for the two parts
// that were visibly wrong here:
//
//   * which image — a recent-window shuffle (CrossPointState's 16-deep ring),
//     not "random, and if it hit the last one, take the next". Names are
//     sorted so an index means the same file from sleep to sleep.
//   * how it is drawn — Atkinson error diffusion on the fitted size, then the
//     panel's 4-level grayscale, same as renderBitmapSleepScreen. Ordered
//     Bayer on top of a nearest-neighbour scale turned photos into newsprint.

namespace {

const char* const SLEEP_DIRS[] = {"/sleep", "/.sleep"};
constexpr char SLEEP_SINGLE_FILE[] = "/sleep.bmp";
constexpr int MAX_SLEEP_IMAGES = 64;
constexpr size_t MAX_SLEEP_NAME = 64;

constexpr uint32_t SLEEP_RTC_MAGIC = 0x534C5032;  // "SLP2" — layout of the ring changed
RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR SleepRecent rtcRecent = {};

bool hasBmpExtension(const char* name) {
  const size_t n = strlen(name);
  return n > 4 && strcasecmp(name + n - 4, ".bmp") == 0;
}

bool isDrawableBmp(FsFile& file) {
  Bitmap probe(file);
  const bool ok = probe.parseHeaders() == BmpReaderError::Ok;
  file.rewind();
  return ok;
}

int cmpName(const void* a, const void* b) {
  return strcasecmp(static_cast<const char*>(a), static_cast<const char*>(b));
}

// Sorted list of drawable BMP names. A fixed cap keeps this off a growing
// vector; a personal sleep folder is nowhere near 64 images.
int listDrawable(const char* dirPath, char names[][MAX_SLEEP_NAME], const int cap) {
  auto dir = SdMan.open(dirPath);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return 0;
  }

  int count = 0;
  dir.rewindDirectory();
  for (auto file = dir.openNextFile(); file && count < cap; file = dir.openNextFile()) {
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    file.getName(names[count], MAX_SLEEP_NAME);
    const bool candidate = names[count][0] != '\0' && names[count][0] != '.' && hasBmpExtension(names[count]);
    const bool drawable = candidate && isDrawableBmp(file);
    file.close();
    if (drawable) count++;
  }
  dir.close();
  if (count > 1) qsort(names, static_cast<size_t>(count), MAX_SLEEP_NAME, cmpName);
  return count;
}

int chooseIndex(const SleepScreenMode mode, const int count) {
  const bool haveHistory = (rtcMagic == SLEEP_RTC_MAGIC);
  const int next = sleepScreenNextIndex(mode, count, haveHistory, rtcRecent, esp_random());
  sleepScreenRemember(rtcRecent, static_cast<uint16_t>(next));
  rtcMagic = SLEEP_RTC_MAGIC;
  return next;
}

// Photos go through the 4-level grayscale waveform. 1-bit art stays a single
// black-and-white refresh so a crisp drawing is not softened.
bool presentBitmap(GfxRenderer& renderer, Bitmap& bitmap) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const SleepPlacement place = sleepScreenFit(bitmap.getWidth(), bitmap.getHeight(), screenW, screenH);
  if (place.w <= 0 || place.h <= 0) return false;

  if (bitmap.getWidth() > place.w || bitmap.getHeight() > place.h) {
    bitmap.setDitheredOutputSize(place.w, place.h);
  }

  renderer.clearScreen();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.drawBitmap(bitmap, place.x, place.y, screenW, screenH);

  if (!bitmap.hasGreyscale()) {
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return true;
  }

  // The grayscale LUT is a fast differential update. Without a real base
  // refresh first it just fades whatever is already on the glass — the
  // screen looks frozen and washed out, and the wallpaper never appears.
  // CrossInk does this with displayGrayscaleBase(HALF_REFRESH).
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
    if (bitmap.rewindToData() != BmpReaderError::Ok) {
      renderer.setRenderMode(GfxRenderer::BW);
      return false;
    }
    // Gray planes are white marks on a black field. clearScreen() leaves
    // 0xFF, and drawing white onto white would store an empty plane.
    renderer.clearScreen(0x00);
    renderer.setRenderMode(mode);
    renderer.drawBitmap(bitmap, place.x, place.y, screenW, screenH);
    if (mode == GfxRenderer::GRAYSCALE_LSB) {
      renderer.copyGrayscaleLsbBuffers();
    } else {
      renderer.copyGrayscaleMsbBuffers();
    }
  }
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayGrayBuffer();
  return true;
}

}  // namespace

bool sleepScreenDrawImage(GfxRenderer& renderer, const SleepScreenMode mode) {
  if (mode == SleepScreenMode::TEXT) return false;

  char path[MAX_SLEEP_NAME + 16];
  path[0] = '\0';

  auto* names = static_cast<char(*)[MAX_SLEEP_NAME]>(malloc(MAX_SLEEP_IMAGES * MAX_SLEEP_NAME));
  if (names) {
    for (const char* candidate : SLEEP_DIRS) {
      const int count = listDrawable(candidate, names, MAX_SLEEP_IMAGES);
      if (count <= 0) continue;
      const int index = chooseIndex(mode, count);
      snprintf(path, sizeof(path), "%s/%s", candidate, names[index]);
      break;
    }
    free(names);
  }

  if (path[0] == '\0') {
    if (!SdMan.exists(SLEEP_SINGLE_FILE)) return false;
    snprintf(path, sizeof(path), "%s", SLEEP_SINGLE_FILE);
  }

  auto file = SdMan.open(path);
  if (!file) return false;

  // imageLevels: the grayscale LUT wants even 0/85/170/255 steps, which is
  // what CrossInk uses when the panel can show the four levels.
  Bitmap bitmap(file, true, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) {
    file.close();
    return false;
  }

  const bool drawn = presentBitmap(renderer, bitmap);
  file.close();
  SdMan.sleep();
  return drawn;
}
