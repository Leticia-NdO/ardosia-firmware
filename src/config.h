#pragma once

#include <cstdint>
#include <cstddef>

// --- UI State Machine ---
enum class UIState {
  MAIN_MENU,
  FILE_BROWSER,
  TEXT_EDITOR,
  RENAME_FILE,
  NEW_FILE,
  SETTINGS,
  BLUETOOTH_SETTINGS,
  PAIRED_KEYBOARDS,
  WIFI_SYNC
};

// --- Display Orientation ---
// Values map to GfxRenderer::Orientation enum
enum class Orientation : uint8_t {
  PORTRAIT = 0,
  LANDSCAPE_CW = 1,    // LandscapeClockwise
  PORTRAIT_INV = 2,     // PortraitInverted
  LANDSCAPE_CCW = 3     // LandscapeCounterClockwise
};

// --- Writing Modes ---
enum class WritingMode : uint8_t {
  NORMAL     = 0,   // Standard scrolling editor
  TYPEWRITER = 1,   // Shows only current line centered on screen
  PAGINATION = 2    // Page-based display instead of scrolling
};

// --- BLE Connection State ---
enum class BLEState : uint8_t {
  DISCONNECTED,
  SCANNING,
  CONNECTING,
  CONNECTED
};

// --- Key Event (for input queue) ---
struct KeyEvent {
  uint8_t keyCode;
  uint8_t modifiers;
  bool pressed;
};

// --- File Info ---
static constexpr int MAX_FILENAME_LEN = 64;
static constexpr int MAX_TITLE_LEN = 40;

struct FileInfo {
  char filename[MAX_FILENAME_LEN];
  char title[MAX_TITLE_LEN];
  unsigned long modTime;
};

// --- Auto-save timing ---
static constexpr unsigned long AUTO_SAVE_IDLE_MS = 10000;    // Save after 10s of no keystrokes
static constexpr unsigned long AUTO_SAVE_MAX_MS  = 120000;   // Hard cap: save every 2min during continuous typing

// --- OTA App Detection ---
static constexpr int MAX_OTA_APPS = 4;

struct OtaAppEntry {
  char name[32];
  int partitionSubtype;
};

extern OtaAppEntry otaApps[];
extern int otaAppCount;

// --- Buffer/Queue Sizes ---
static constexpr size_t TEXT_BUFFER_SIZE = 16384;
static constexpr int MAX_FILES = 50;
static constexpr int INPUT_QUEUE_SIZE = 50;
static constexpr int MAX_LINES = 1024;

// --- Font IDs (from crosspoint-reader fontIds.h) ---
#define FONT_BODY    (-1014561631)   // NOTOSANS_14_FONT_ID
#define FONT_UI      (-1559651934)   // NOTOSANS_12_FONT_ID
#define FONT_SMALL   (-1246724383)   // UI_10_FONT_ID (ubuntu 10)
#define FONT_LARGE   (-1422711852)   // NOTOSANS_16_FONT_ID

// --- Keyboard layout ---
// A BLE keyboard reports keys by PHYSICAL POSITION, not by what is printed on
// the keycap, so the layout cannot be detected — it has to be told. Usage 0x34
// is the apostrophe on US ANSI and the ~/^ key on ABNT2; reading an ABNT2 board
// with the US map is what turns a ~ keypress into ä.
//
//   US       plain US ANSI: ' " ` ~ ^ are literal characters
//   US_INTL  US ANSI with dead keys, the only way to reach á ã ç on a board
//            that has no accent keys ('a -> á, 'c -> ç)
//   ABNT2    Brazilian ISO board: real Ç key, ´` and ~^ dead keys
enum class KeyboardLayout : uint8_t { US = 0, US_INTL = 1, ABNT2 = 2 };

// --- Sleep screen ---
// TEXT is the built-in "MicroSlate / Asleep" card. The others draw a BMP from
// the SD card (/sleep/, /.sleep/, or /sleep.bmp) and fall back to TEXT when
// there is nothing usable to draw.
enum class SleepScreenMode : uint8_t {
  TEXT      = 0,
  SLIDESHOW = 1,   // next image in the folder, each time it sleeps
  SHUFFLE   = 2    // random image, never the same one twice running
};

// --- Sleep wallpaper tone ---
// How dark the halftone of a wallpaper comes out. See sleep_layout.cpp: the
// panel is 1-bit, so tone is density of black dots, and this picks how dense.
enum class SleepBrightness : uint8_t { NORMAL = 0, LIGHT = 1, LIGHTER = 2 };

// --- Font Size ---
enum class FontSize : uint8_t { SMALL = 0, MEDIUM = 1, LARGE = 2 };

inline int editorFontId(FontSize size) {
  switch (size) {
    case FontSize::SMALL:  return FONT_UI;    // notosans 12
    case FontSize::MEDIUM: return FONT_BODY;  // notosans 14
    default:               return FONT_LARGE; // notosans 16
  }
}

// --- HID Keycodes ---
static constexpr uint8_t HID_KEY_A          = 0x04;
static constexpr uint8_t HID_KEY_B          = 0x05;
static constexpr uint8_t HID_KEY_D          = 0x07;
static constexpr uint8_t HID_KEY_F          = 0x09;
static constexpr uint8_t HID_KEY_N          = 0x11;
static constexpr uint8_t HID_KEY_P          = 0x13;
static constexpr uint8_t HID_KEY_Q          = 0x14;
static constexpr uint8_t HID_KEY_R          = 0x15;
static constexpr uint8_t HID_KEY_W          = 0x1A;
static constexpr uint8_t HID_KEY_S          = 0x16;
static constexpr uint8_t HID_KEY_T          = 0x17;
static constexpr uint8_t HID_KEY_Z          = 0x1D;
static constexpr uint8_t HID_KEY_ENTER      = 0x28;
static constexpr uint8_t HID_KEY_ESCAPE     = 0x29;
static constexpr uint8_t HID_KEY_BACKSPACE  = 0x2A;
static constexpr uint8_t HID_KEY_TAB        = 0x2B;
static constexpr uint8_t HID_KEY_SPACE      = 0x2C;
static constexpr uint8_t HID_KEY_DELETE     = 0x4C;
static constexpr uint8_t HID_KEY_RIGHT      = 0x4F;
static constexpr uint8_t HID_KEY_LEFT       = 0x50;
static constexpr uint8_t HID_KEY_DOWN       = 0x51;
static constexpr uint8_t HID_KEY_UP         = 0x52;
static constexpr uint8_t HID_KEY_HOME       = 0x4A;
static constexpr uint8_t HID_KEY_END        = 0x4D;
static constexpr uint8_t HID_KEY_CAPSLOCK   = 0x39;
static constexpr uint8_t HID_KEY_F2         = 0x3B;

// --- HID Modifier Masks ---
static constexpr uint8_t MOD_CTRL_LEFT   = 0x01;
static constexpr uint8_t MOD_SHIFT_LEFT  = 0x02;
static constexpr uint8_t MOD_ALT_LEFT    = 0x04;
static constexpr uint8_t MOD_CTRL_RIGHT  = 0x10;
static constexpr uint8_t MOD_SHIFT_RIGHT = 0x20;
static constexpr uint8_t MOD_ALT_RIGHT   = 0x40;

inline bool isCtrl(uint8_t mod) {
  return (mod & MOD_CTRL_LEFT) || (mod & MOD_CTRL_RIGHT);
}
inline bool isShift(uint8_t mod) {
  return (mod & MOD_SHIFT_LEFT) || (mod & MOD_SHIFT_RIGHT);
}

// --- Debug Logging ---
// Define RELEASE_BUILD in platformio.ini to disable all serial output.
// This saves significant power by keeping the UART peripheral inactive.
#ifdef RELEASE_BUILD
  #define DBG_INIT()
  #define DBG_PRINTF(fmt, ...)
  #define DBG_PRINTLN(s)
  #define DBG_PRINT(s)
#else
  #define DBG_INIT()            Serial.begin(115200)
  #define DBG_PRINTF(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__)
  #define DBG_PRINTLN(s)        Serial.println(s)
  #define DBG_PRINT(s)          Serial.print(s)
#endif
