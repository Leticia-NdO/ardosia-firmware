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

// A note whose word count has never been computed. Distinct from 0, which is a
// real answer: an empty note. Notes that predate the count, or that arrived over
// sync or on a card from another device, carry this until they are first saved.
static constexpr uint32_t NOTE_WORDS_UNKNOWN = 0xFFFFFFFFu;

struct FileInfo {
  char filename[MAX_FILENAME_LEN];
  char title[MAX_TITLE_LEN];
  unsigned long modTime;
  uint32_t words;
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
static constexpr int MAX_FILES = 100;
// POST /notes rolls to title_2.txt once the current file reaches this.
// Kept under TEXT_BUFFER_SIZE so the editor can still open the result.
static constexpr size_t NOTE_ROLLOVER_SIZE = 14 * 1024;
static constexpr size_t NOTE_POST_MAX_BODY = 32 * 1024;

// Main menu: keep these in lockstep with the label array in drawMainMenu
// and the switch in dispatchEvent. A bare `4` used to appear in five places.
//
// --- TEMPORARY: the STA Sync entry is hidden from the menu ---------------
// Flip this back to true to restore it. Nothing else needs touching: the
// label, the handler, wifi_sync.cpp and the WIFI_SYNC screen are all still
// built, only unreachable from the menu.
//
// Hidden rather than deleted because the feature works — what is unresolved is
// what it should be. Its PC-side installers for macOS/Linux do not exist and
// its default download folder is a Windows OneDrive path.
//
// Hotspot stays: it is the other direction (the device raises the AP and
// accepts POST /notes), and it is the X3 clippings path from SPEC-clippings.md.
static constexpr bool MENU_SHOW_SYNC = false;

// The order here is the order on screen, and Sync sits last precisely because
// it is the entry that comes and goes — an optional row in the middle would
// renumber everything below it each time it is toggled.
static constexpr int MENU_BROWSE     = 0;
static constexpr int MENU_NEW_NOTE   = 1;
static constexpr int MENU_SETTINGS   = 2;
static constexpr int MENU_SYNC_AP    = 3;   // drawn as "Hotspot"

// -1 while hidden, on purpose. mainMenuSelection is never negative, so the
// dispatch branch simply never matches. Leaving it at 4 would be a real bug:
// with BASE_MENU_COUNT down to 4, index 4 is the first OTA app, and
// `selection == MENU_SYNC` is tested before the OTA branch — so Enter on
// CrossPoint would raise the radio instead of rebooting into the reader.
static constexpr int MENU_SYNC       = MENU_SHOW_SYNC ? 4 : -1;
static constexpr int BASE_MENU_COUNT = MENU_SHOW_SYNC ? 5 : 4;
// Settings rows. These numbers are IDENTITY ONLY — they say which setting a row
// is, not where it appears. Since Settings grew tabs, the order on screen is
// the order of settingsTabRows() below, and a row can be moved between tabs or
// within one without touching anything here or in the handlers that compare
// against these constants.
//
// They stay contiguous from 0 all the same: settingLabel() is a switch over
// them, and the tests sweep the whole range to prove no row lost its label or
// its place in a tab.
static constexpr int SET_DARK_MODE   = 0;
static constexpr int SET_EDITOR_FONT = 1;
static constexpr int SET_FONT_SIZE   = 2;
static constexpr int SET_WRITING     = 3;
static constexpr int SET_NOTE_ORDER  = 4;
static constexpr int SET_SLEEP       = 5;
static constexpr int SET_ORIENTATION = 6;
static constexpr int SET_KEYBOARD    = 7;
static constexpr int SET_BLUETOOTH   = 8;
static constexpr int SET_PAIRED_KB   = 9;
static constexpr int SET_DPAD        = 10;
static constexpr int SETTINGS_COUNT  = 11;

// The label belongs next to the index, not in a parallel array a few hundred
// lines away: reordering the rows used to mean reordering both by hand, and
// getting that half-right shows the wrong label on a row that still cycles the
// old setting — silently. A switch cannot drift.
inline const char* settingLabel(int row) {
  switch (row) {
    case SET_DARK_MODE:   return "Dark Mode";
    case SET_EDITOR_FONT: return "Editor Font";
    case SET_FONT_SIZE:   return "Font Size";
    case SET_WRITING:     return "Writing Mode";
    case SET_NOTE_ORDER:  return "Note Order";
    case SET_SLEEP:       return "Sleep Screen";
    case SET_ORIENTATION: return "Orientation";
    case SET_KEYBOARD:    return "Keyboard";
    case SET_BLUETOOTH:   return "Bluetooth";
    case SET_PAIRED_KB:   return "Paired Keyboards";
    case SET_DPAD:        return "D-Pad Keys";
    default:              return "";
  }
}

// --- Settings tabs ---------------------------------------------------------
//
// Eleven rows against a landscape screen that holds seven, and ten digits
// against eleven rows: the flat list had outgrown both. Three tabs fix both at
// once. No tab holds more than five rows, so nothing scrolls in landscape, and
// the digits restart inside each tab — which gives D-Pad Keys, the row that had
// to go without a number, its number back.
//
// Grouping follows the design in prototypes/settings-with-tabs.png:
//
//   System    the device itself: what it looks like and how it sleeps
//   Editor    writing and the note list
//   Controls  everything that takes input — the keyboard, the radio it
//             arrives on, and the buttons on the case
//
// Two calls worth stating, because neither is obvious:
//
//   Dark Mode is System, not Editor. It inverts every screen (ui_renderer
//   takes `tc = !darkMode` for all of them), not just the page you type on.
//
//   Note Order is Editor. It orders the notes list rather than the editor, so
//   the honest home for it would be a fourth "Notes" tab — which three rows do
//   not justify. Editor is where writing lives, and the note list is part of
//   writing.
//
// Order INSIDE a tab is by how often a row is opened. Font Size and Writing
// Mode sit below Editor Font despite being adjusted more, because Ctrl+F,
// Ctrl+T and Ctrl+P already reach them from the editor: Settings is their
// fallback, not their front door.
static constexpr int TAB_SYSTEM   = 0;
static constexpr int TAB_EDITOR   = 1;
static constexpr int TAB_CONTROLS = 2;
static constexpr int SETTINGS_TAB_COUNT = 3;

inline const char* settingsTabLabel(int tab) {
  switch (tab) {
    case TAB_SYSTEM:   return "System";
    case TAB_EDITOR:   return "Editor";
    case TAB_CONTROLS: return "Controls";
    default:           return "";
  }
}

// The same three names with the width taken out. Portrait is 320px narrower
// than landscape and the full names very nearly fill it; the bar measures
// itself and falls back to these rather than dropping a tab, which would put a
// whole group of settings out of reach on a device with no other way in.
inline const char* settingsTabShortLabel(int tab) {
  switch (tab) {
    case TAB_SYSTEM:   return "Sys";
    case TAB_EDITOR:   return "Edit";
    case TAB_CONTROLS: return "Ctrl";
    default:           return "";
  }
}

// THE table. Everything else about tabs is derived from it, so a row can only
// be in one place and cannot be in none — a row left out of here would vanish
// from the UI of a device with no serial console and no other way to reach it.
// The tests sweep it for exactly that.
inline const int* settingsTabRows(int tab, int& count) {
  static const int kSystem[]   = {SET_DARK_MODE, SET_ORIENTATION, SET_SLEEP};
  static const int kEditor[]   = {SET_EDITOR_FONT, SET_FONT_SIZE, SET_WRITING,
                                  SET_NOTE_ORDER};
  static const int kControls[] = {SET_BLUETOOTH, SET_PAIRED_KB, SET_KEYBOARD,
                                  SET_DPAD};
  switch (tab) {
    case TAB_SYSTEM:   count = 3; return kSystem;
    case TAB_EDITOR:   count = 4; return kEditor;
    case TAB_CONTROLS: count = 4; return kControls;
    default:           count = 0; return nullptr;
  }
}

inline int settingsTabRowCount(int tab) {
  int n = 0;
  settingsTabRows(tab, n);
  return n;
}

// The SET_* row at `pos` inside `tab`, or -1 when there is none.
inline int settingsTabRow(int tab, int pos) {
  int n = 0;
  const int* rows = settingsTabRows(tab, n);
  return (rows && pos >= 0 && pos < n) ? rows[pos] : -1;
}

// Where a row lives. {-1, -1} for a row no tab holds, which the tests assert
// never happens for a real row.
struct SettingsPlace {
  int tab;
  int pos;
};

inline SettingsPlace settingsPlaceOf(int row) {
  for (int tab = 0; tab < SETTINGS_TAB_COUNT; tab++) {
    int n = 0;
    const int* rows = settingsTabRows(tab, n);
    for (int pos = 0; pos < n; pos++) {
      if (rows[pos] == row) return SettingsPlace{tab, pos};
    }
  }
  return SettingsPlace{-1, -1};
}

static constexpr int INPUT_QUEUE_SIZE = 50;
static constexpr int MAX_LINES = 1024;

// --- Font IDs (from crosspoint-reader fontIds.h) ---
// These three are the EDITOR's fonts. editorFontId() below maps the user's
// Font Size setting onto them, so they must keep pointing at NotoSans even
// after the chrome moved to a monospace face.
//
// A fourth, FONT_SMALL (ubuntu 10), used to sit here. Once the chrome moved to
// JetBrains Mono nothing drew with it, so the font left the build.
#define FONT_BODY    (-1014561631)   // NOTOSANS_14_FONT_ID
#define FONT_UI      (-1559651934)   // NOTOSANS_12_FONT_ID
#define FONT_LARGE   (-1422711852)   // NOTOSANS_16_FONT_ID

// --- Chrome font IDs (JetBrains Mono) ---
// Menus, headers, footers and lists. Separate from the editor's on purpose:
// editorFontId() reuses FONT_UI/FONT_BODY/FONT_LARGE, so repointing those at
// the monospace face would drag the body of the note along with the chrome.
//
// SIZES ARE IN POINTS AT 150 DPI, not pixels — fontconvert.py does
// `face.set_char_size(size << 6, size << 6, 150, 150)`, so a pixel size is
// pt x 150/72. The Figma canvas is the panel's own 800x480, so its px values
// are device pixels and convert back the other way, pt = px x 72/150:
//
//     design 11px -> 5pt      design 15px -> 7pt
//     design 13px -> 6pt      design 22px -> 11pt
//
// Getting this backwards is how the first cut came out at roughly double the
// intended size: jetbrainsmono_15 is a 31px font, not a 15px one.
//
// The value is only a key into GfxRenderer's font map, so unlike the editor's
// IDs it is not a hash of anything. build-font-ids.sh derives those from SHA256
// of the header files; that script is vendored from crosspoint and stays byte
// for byte, so these are hand-picked instead: 'MON' in the high bytes, the
// point size in the low one. Nothing can collide — the editor's are negative.
// The sizes are NOT the Figma's 11/13/15/22px. Drawn 1:1 the design came out
// too small to read on this panel, so the text scales ~1.8x while the padding
// deliberately does not — see the note on ROW_PAD in ui_layout.h. The row label
// lands back on 31px of ink, the size that was on the device when the chrome
// first went monospace.
#define FONT_CHROME_S  (0x4D4F4E09)   // 9pt,  ink 23 — footer, status line
#define FONT_CHROME_M  (0x4D4F4E0B)   // 11pt, ink 27 — digits, counts, secondary
#define FONT_CHROME_L  (0x4D4F4E0D)   // 13pt, ink 31 — row labels, note titles
#define FONT_CHROME_XL (0x4D4F4E12)   // 18pt, ink 43 — screen title in the header

// --- Monospace editor faces ---
// The editor's own three sizes in JetBrains Mono, for Editor Font: Mono. Same
// point sizes as the NotoSans they stand in for, so Font Size means the same
// thing whichever face is picked.
//
// Regular only, no bold: the editor never draws a styled span, and
// EpdFontFamily::getFont() falls back to regular when the bold slot is null
// (EpdFontFamily.cpp:18). Three faces instead of six, for half the flash.
#define FONT_EDIT_MONO_S (0x4D4F4E0C)   // 12pt — Font Size: Small
#define FONT_EDIT_MONO_M (0x4D4F4E0E)   // 14pt — Font Size: Medium
#define FONT_EDIT_MONO_L (0x4D4F4E10)   // 16pt — Font Size: Large

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
// TEXT is the built-in "Ardosia / Asleep" card. The others draw a BMP from
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

// How the notes list is ordered. Creation is a counter we assign, not the
// FAT timestamp: this board has no wall clock, so device-created files
// would all share the 1980 epoch and sort as a tie.
enum class NoteSort : uint8_t {
  ALPHA_ASC = 0,
  ALPHA_DESC = 1,
  NEWEST = 2,
  OLDEST = 3
};

// Which typeface the note body is set in. The chrome is always monospace; this
// is only about the text being written.
enum class EditorFont : uint8_t { SANS = 0, MONO = 1 };

inline int editorFontId(FontSize size, EditorFont face) {
  if (face == EditorFont::MONO) {
    switch (size) {
      case FontSize::SMALL:  return FONT_EDIT_MONO_S;
      case FontSize::MEDIUM: return FONT_EDIT_MONO_M;
      default:               return FONT_EDIT_MONO_L;
    }
  }
  switch (size) {
    case FontSize::SMALL:  return FONT_UI;    // notosans 12
    case FontSize::MEDIUM: return FONT_BODY;  // notosans 14
    default:               return FONT_LARGE; // notosans 16
  }
}

// --- HID Keycodes ---
static constexpr uint8_t HID_KEY_A          = 0x04;
static constexpr uint8_t HID_KEY_B          = 0x05;
static constexpr uint8_t HID_KEY_C          = 0x06;
static constexpr uint8_t HID_KEY_D          = 0x07;
static constexpr uint8_t HID_KEY_F          = 0x09;
static constexpr uint8_t HID_KEY_K          = 0x0E;
static constexpr uint8_t HID_KEY_N          = 0x11;
static constexpr uint8_t HID_KEY_P          = 0x13;
static constexpr uint8_t HID_KEY_Q          = 0x14;
static constexpr uint8_t HID_KEY_R          = 0x15;
static constexpr uint8_t HID_KEY_W          = 0x1A;
static constexpr uint8_t HID_KEY_S          = 0x16;
static constexpr uint8_t HID_KEY_T          = 0x17;
static constexpr uint8_t HID_KEY_V          = 0x19;
static constexpr uint8_t HID_KEY_X          = 0x1B;
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
// The function row, as far as the Settings tabs need it. Positional like every
// other usage, and in the same place on all three layouts.
static constexpr uint8_t HID_KEY_F1         = 0x3A;
static constexpr uint8_t HID_KEY_F2         = 0x3B;
static constexpr uint8_t HID_KEY_F3         = 0x3C;

// Number row. These usages are positional like every other HID code, but here
// that works in our favour: 0x1E is the leftmost key of the number row on US,
// US-Intl and ABNT2 alike, so a menu digit shortcut needs no layout lookup and
// cannot be broken by the keyboard setting. 0 sits AFTER 9, as on the keycaps.
static constexpr uint8_t HID_KEY_1          = 0x1E;
static constexpr uint8_t HID_KEY_9          = 0x26;
static constexpr uint8_t HID_KEY_0          = 0x27;

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
inline bool isAlt(uint8_t mod) {
  return (mod & MOD_ALT_LEFT) || (mod & MOD_ALT_RIGHT);
}

// --- Menu digit shortcuts ---
// Menus label each row with the digit that jumps to it ("3. Settings"). This
// maps a key press to that 1-based row number, or 0 when the press is not a
// menu shortcut.
//
// Any modifier disqualifies it: Shift+1 is "!", AltGr+1 is "¹" on ABNT2, and
// Ctrl+1 is free to mean something else later. A bare digit is the shortcut.
//
// Only the number row, not the keypad: a keypad sends 0x59..0x62 for both the
// digits and the arrows/Home/End printed on the same keys, and which one it
// means depends on NumLock — state this firmware does not track. Reading them
// as digits would make a numpad arrow jump the selector. The target keyboards
// (Keys-To-Go 2, K3) have no keypad anyway.
//
// 0 selects the TENTH row, the way a keypad menu has always numbered itself:
// 0 sits after 9 on the keycaps and on the HID usage table alike. It used to be
// excluded, with the note that no menu here reached ten — adding Editor Font to
// Settings is exactly the day that stopped being true.
//
// Returns the 1-based row, so 0 still means "not a shortcut".
inline int menuDigit(uint8_t hid, uint8_t mod) {
  if (isCtrl(mod) || isShift(mod) || isAlt(mod)) return 0;
  if (hid == HID_KEY_0) return 10;
  if (hid < HID_KEY_1 || hid > HID_KEY_9) return 0;
  return (hid - HID_KEY_1) + 1;
}

// Rows past this have no digit to type, so they get no number drawn either —
// a number on screen that no key produces is worse than no number.
static constexpr int MENU_MAX_NUMBERED = 10;

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
