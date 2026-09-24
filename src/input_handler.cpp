#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "utf8_util.h"
#include "deadkeys.h"
#include "keymap.h"
#include "dpad.h"
#include "confirm.h"
#include "quickmenu.h"

#include <Arduino.h>
#include <SDCardManager.h>
#include <cstring>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern ConfirmDialog confirmDialog;
extern QuickMenu quickMenu;
extern WritingMode writingMode;
extern FontSize fontSize;
extern EditorFont editorFont;
extern DpadMode dpadMode;
extern bool showWordCount;

// External functions
void storePairedDevice(const std::string& address, const std::string& name);
bool getStoredDevice(std::string& address, std::string& name);
void clearStoredDevice();
uint32_t getCurrentPasskey();
bool isDeviceScanning();
void refreshScanNow();
void clearAllBluetoothBonds();

// --- Input Queue ---
static KeyEvent inputQueue[INPUT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;
static volatile bool queueFull = false;

// --- CapsLock state ---
static bool capsLockOn = false;

// --- Keyboard layout (defined in main.cpp, persisted in NVS) ---
extern KeyboardLayout keyboardLayout;
extern SleepScreenMode sleepScreenMode;

// --- Last key seen, shown in the Settings footer -----------------------------
// A BLE keyboard reports HID usage codes by physical POSITION, not by what is
// printed on the keycap: usage 0x35 is "the key left of 1" whatever its legend
// says. So a keyboard that is not US ANSI lands different characters on the
// same codes, and the only way to tell that apart from a mapping bug — on a
// device with no serial console — is to show the raw code that arrived.
//
// Permanent, not diagnostic scaffolding: this device pairs with arbitrary
// keyboards and has no other way to identify one.
static uint8_t lastKeyHid = 0;
static uint8_t lastKeyMods = 0;

void inputGetLastKey(uint8_t* hid, uint8_t* mods) {
  if (hid) *hid = lastKeyHid;
  if (mods) *mods = lastKeyMods;
}

// Where to return after title edit is confirmed or cancelled
static UIState renameReturnState = UIState::FILE_BROWSER;

// What the title editor is editing. This used to be inferred from
// renameReturnState, which held up while there were exactly two callers and
// stopped the moment a third arrived — renaming an OTA app also returns to the
// main menu, so the return state no longer identifies the target.
enum class RenameTarget : uint8_t { NoteTitle, OtaAppName };
static RenameTarget renameTarget = RenameTarget::NoteTitle;
static int renameOtaIndex = -1;

bool renameOtaApp(int index, const char* name);

// Forward declaration
static void openTitleEdit(const char* currentTitle, UIState returnTo,
                          RenameTarget target = RenameTarget::NoteTitle,
                          int otaIndex = -1);
static void quickMenuAct(QuickItem item, int index);

// OTA app detection (defined in main.cpp)
extern OtaAppEntry otaApps[];
extern int otaAppCount;
void switchToOtaApp(int index);

// --- Shared UI state (defined in main.cpp) ---
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int settingsTab;
extern NoteSort noteSort;
extern int bluetoothDeviceSelection;
extern int pairedKeyboardSelection;
extern Orientation currentOrientation;
extern int charsPerLine;
extern bool screenDirty;
extern char renameBuffer[];
extern int renameBufferLen;

void inputSetup() {
  queueHead = 0;
  queueTail = 0;
  queueFull = false;
  capsLockOn = false;
}

static bool isQueueEmpty() {
  return (queueHead == queueTail) && !queueFull;
}

void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed) {
  noInterrupts();
  if (!queueFull) {
    inputQueue[queueHead].keyCode = keyCode;
    inputQueue[queueHead].modifiers = modifiers;
    inputQueue[queueHead].pressed = pressed;
    queueHead = (queueHead + 1) % INPUT_QUEUE_SIZE;
    if (queueHead == queueTail) queueFull = true;
  }
  interrupts();
}

static KeyEvent dequeueKeyEvent() {
  KeyEvent event = {0, 0, false};
  noInterrupts();
  if (!isQueueEmpty()) {
    event = inputQueue[queueTail];
    queueTail = (queueTail + 1) % INPUT_QUEUE_SIZE;
    queueFull = false;
  }
  interrupts();
  return event;
}

char hidToAscii(const uint8_t hid, const uint8_t modifiers) {
  const KeyStroke k = keymapResolve(keyboardLayout, hid, modifiers, capsLockOn);
  // ASCII only, and dead keys report their own character: callers that use this
  // (the WiFi password field) want raw text, not composition.
  return (k.cp != 0 && k.cp < 0x80) ? static_cast<char>(k.cp) : 0;
}

uint32_t inputGetPendingDeadKey() {
  return deadKeyPending();
}

void inputClearDeadKey() {
  deadKeyReset();
}

int inputResolveText(const uint8_t hid, const uint8_t modifiers, uint32_t out[2]) {
  return deadKeyFeed(keymapResolve(keyboardLayout, hid, modifiers, capsLockOn), out);
}

// Human-readable description of the last key, for the Settings footer.
void inputDescribeLastKey(char* buf, const size_t n) {
  if (lastKeyHid == 0) {
    snprintf(buf, n, "Key: press any key to identify it");
    return;
  }
  const KeyStroke k = keymapResolve(keyboardLayout, lastKeyHid, lastKeyMods, capsLockOn);

  char shown[12];
  if (k.cp == 0)                      snprintf(shown, sizeof(shown), "-");
  else if (k.cp == '\n')              snprintf(shown, sizeof(shown), "Enter");
  else if (k.cp == '\t')              snprintf(shown, sizeof(shown), "Tab");
  else if (k.cp == ' ')               snprintf(shown, sizeof(shown), "Space");
  else if (k.cp < 0x80)               snprintf(shown, sizeof(shown), "%c", (char)k.cp);
  else                                snprintf(shown, sizeof(shown), "U+%04lX", (unsigned long)k.cp);

  snprintf(buf, n, "Key: 0x%02X mod:0x%02X -> %s%s",
           lastKeyHid, lastKeyMods, shown, k.mark != DeadMark::None ? " (dead)" : "");
}


// Handle text editor input
static void handleEditorKey(uint8_t keyCode, uint8_t modifiers) {
  // Both boxes are modal, and both are up over live text: a key that fell
  // through here would be typed into the note behind them.
  if (quickMenu.open()) {
    const QuickResult r = quickKey(quickMenu, keyCode, modifiers);
    if (r.reply == QuickReply::Chosen) quickMenuAct(r.item, quickMenu.index);
    if (r.reply != QuickReply::Swallowed) screenDirty = true;
    return;
  }
  if (confirmDialog.open()) {
    const ConfirmReply reply = confirmKey(confirmDialog, keyCode, modifiers);
    if (reply == ConfirmReply::Confirmed) deleteOpenNote(&selectedFileIndex);
    if (reply != ConfirmReply::Swallowed) screenDirty = true;
    return;
  }

  // Ctrl shortcuts
  if (isCtrl(modifiers)) {
    if (keyCode == HID_KEY_S) {
      saveCurrentFile();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_Z) {
      cleanMode = !cleanMode;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_K) {
      // The same menu holding select raises. Worth having from the keyboard
      // too: it is the one place that shows what the font, the mode and the
      // theme currently ARE — every chord below changes a value blind.
      quickOpen(quickMenu, QuickKind::Editor, -1);
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_N) {
      openTitleEdit(editorGetCurrentTitle(), UIState::TEXT_EDITOR);
      return;
    }
    if (keyCode == HID_KEY_T) {
      writingMode = (writingMode == WritingMode::TYPEWRITER) ? WritingMode::NORMAL : WritingMode::TYPEWRITER;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_F) {
      int v = static_cast<int>(fontSize);
      fontSize = static_cast<FontSize>((v + 1) % 3);
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_W) {
      showWordCount = !showWordCount;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_P) {
      writingMode = (writingMode == WritingMode::PAGINATION) ? WritingMode::NORMAL : WritingMode::PAGINATION;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_A) {
      editorSelectAll();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_C) {
      editorCopy();
      return;
    }
    if (keyCode == HID_KEY_X) {
      editorCut();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_V) {
      editorPaste();
      screenDirty = true;
      return;
    }
    // Ctrl+Left/Right: jump pages in pagination mode
    if (writingMode == WritingMode::PAGINATION) {
      int pageSize = editorGetStoredVisibleLines();
      if (keyCode == HID_KEY_LEFT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorUp();
        screenDirty = true;
        return;
      }
      if (keyCode == HID_KEY_RIGHT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorDown();
        screenDirty = true;
        return;
      }
    }
    return;
  }

  // ESC = save and return to file browser
  if (keyCode == HID_KEY_ESCAPE) {
    inputClearDeadKey();
    if (editorHasUnsavedChanges()) saveCurrentFile();
    currentState = UIState::FILE_BROWSER;
    screenDirty = true;
    return;
  }

  // Tab cycles writing modes
  if (keyCode == HID_KEY_TAB) {
    int v = static_cast<int>(writingMode);
    writingMode = static_cast<WritingMode>((v + 1) % 3);
    screenDirty = true;
    return;
  }

  // Navigation keys.  An armed dead key is invisible state apart from the mark
  // drawn in the cursor, so moving away from the insertion point cancels it
  // instead of letting the accent land on whatever is typed next.
  //
  // Backspace cancels it too, and only that: with an accent pending, backspace
  // is how you undo the accent, not how you delete the letter before it.
  if (keyCode == HID_KEY_BACKSPACE && inputGetPendingDeadKey() != 0) {
    inputClearDeadKey();
    screenDirty = true;
    return;
  }
  // Shift is on this same press. Ctrl never reaches here: the block above
  // already returned, so Ctrl+arrow keeps its old meaning (page turn, or nothing).
  const bool extend = isShift(modifiers);
  switch (keyCode) {
    case HID_KEY_LEFT:      inputClearDeadKey(); editorMoveCursorLeft(extend);  screenDirty = true; return;
    case HID_KEY_RIGHT:     inputClearDeadKey(); editorMoveCursorRight(extend); screenDirty = true; return;
    case HID_KEY_UP:        inputClearDeadKey(); editorMoveCursorUp(extend);    screenDirty = true; return;
    case HID_KEY_DOWN:      inputClearDeadKey(); editorMoveCursorDown(extend);  screenDirty = true; return;
    case HID_KEY_HOME:      inputClearDeadKey(); editorMoveCursorHome(extend);  screenDirty = true; return;
    case HID_KEY_END:       inputClearDeadKey(); editorMoveCursorEnd(extend);   screenDirty = true; return;
    case HID_KEY_BACKSPACE: editorDeleteChar();      screenDirty = true; return;
    case HID_KEY_DELETE:    inputClearDeadKey(); editorDeleteForward();   screenDirty = true; return;
  }

  // CapsLock toggle
  if (keyCode == HID_KEY_CAPSLOCK) {
    capsLockOn = !capsLockOn;
    return;
  }

  // Printable character. inputResolveText() runs the dead-key state machine, so
  // one key press can produce nothing (an accent was armed), one character, or
  // two (an accent that had no composed form, followed by the letter).
  uint32_t cps[2];
  const int produced = inputResolveText(keyCode, modifiers, cps);
  for (int i = 0; i < produced; i++) {
    editorInsertCodepoint(cps[i]);
  }
  // Redraw either way: with nothing produced the cursor now shows an armed accent.
  if (produced > 0 || inputGetPendingDeadKey() != 0) {
    screenDirty = true;
  }
}

// Open the title edit screen, returning to `returnTo` on confirm/cancel
const char* renameScreenTitle() {
  return renameTarget == RenameTarget::OtaAppName ? "Rename App" : "Edit Title";
}

bool renameTargetIsApp() { return renameTarget == RenameTarget::OtaAppName; }

static void openTitleEdit(const char* currentTitle, UIState returnTo,
                          RenameTarget target, int otaIndex) {
  inputClearDeadKey();
  renameTarget = target;
  renameOtaIndex = otaIndex;
  strncpy(renameBuffer, currentTitle, MAX_TITLE_LEN - 1);
  renameBuffer[MAX_TITLE_LEN - 1] = '\0';
  utf8TrimPartialTail(renameBuffer);
  renameBufferLen = strlen(renameBuffer);
  renameReturnState = returnTo;
  currentState = UIState::RENAME_FILE;
  screenDirty = true;
}

// Handle title edit input
static void handleRenameKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ENTER || keyCode == HID_KEY_ESCAPE) {
    inputClearDeadKey();
  }
  if (keyCode == HID_KEY_ENTER) {
    if (renameTarget == RenameTarget::OtaAppName) {
      // An empty name is allowed here, unlike a note title: it clears the
      // override and brings the generic "OTA Slot N" back.
      //
      // OtaAppEntry::name holds 32 bytes against the title buffer's 40, so a
      // long name is cut — on a character boundary, not mid-sequence, or a
      // trailing "ç" would become a stray byte in NVS and on the menu.
      char appName[32];
      strncpy(appName, renameBuffer, sizeof(appName) - 1);
      appName[sizeof(appName) - 1] = '\0';
      utf8TrimPartialTail(appName);
      if (!renameOtaApp(renameOtaIndex, appName)) return;
      renameTarget = RenameTarget::NoteTitle;
      currentState = renameReturnState;
      screenDirty = true;
      return;
    }
    if (renameBufferLen > 0) {
      if (renameReturnState == UIState::TEXT_EDITOR) {
        if (editorGetCurrentFile()[0] == '\0') {
          // New file — derive filename from title
          char filename[MAX_FILENAME_LEN];
          if (!deriveUniqueFilename(renameBuffer, filename, MAX_FILENAME_LEN)) {
            return;  // stay on the rename screen; 99 collisions is rare
          }
          editorSetCurrentFile(filename);
        } else {
          // Existing file — rename on disk to match new title
          if (!updateFileTitle(editorGetCurrentFile(), renameBuffer)) {
            return;
          }
        }
        editorSetCurrentTitle(renameBuffer);
        editorSetUnsavedChanges(true);
        saveCurrentFile();
      } else {
        // Updating title of a file selected in the browser
        FileInfo* note = noteVisibleAt(selectedFileIndex);
        if (note == nullptr || !updateFileTitle(note->filename, renameBuffer)) {
          return;
        }
        noteClampSelection(&selectedFileIndex);
      }
    }
    currentState = renameReturnState;
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_ESCAPE) {
    renameTarget = RenameTarget::NoteTitle;
    currentState = renameReturnState;
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_BACKSPACE) {
    if (inputGetPendingDeadKey() != 0) {
      inputClearDeadKey();
      screenDirty = true;
      return;
    }
    if (renameBufferLen > 0) {
      // Step back a whole character, not a byte — "Diário" must not lose half an á.
      renameBufferLen = (int)utf8PrevStart(renameBuffer, (size_t)renameBufferLen);
      renameBuffer[renameBufferLen] = '\0';
      screenDirty = true;
    }
    return;
  }

  // Allow all printable characters in a title (including spaces)
  uint32_t cps[2];
  const int produced = inputResolveText(keyCode, modifiers, cps);
  for (int i = 0; i < produced; i++) {
    if (cps[i] < ' ') continue;                    // no newline or tab in a title
    char enc[4];
    const int len = utf8Encode(cps[i], enc);
    if (len <= 0 || renameBufferLen + len >= MAX_TITLE_LEN) break;
    memcpy(renameBuffer + renameBufferLen, enc, (size_t)len);
    renameBufferLen += len;
    renameBuffer[renameBufferLen] = '\0';
  }
  if (produced > 0 || inputGetPendingDeadKey() != 0) {
    screenDirty = true;
  }
}

// Moving to a tab parks the selector on its first row, rather than remembering
// where the tab was left. Predictable beats clever on a screen you are on
// because you are hunting for something: the same key always lands in the same
// place. It also keeps the invariant the Settings case relies on —
// settingsSelection is always a row of settingsTab.
// What the quick menu's rows actually do. The menu itself only knows which rows
// exist and which one is selected; everything that touches the device is here.
// `index` is the notes-list row the menu was raised on, passed in because the
// menu has already closed itself by the time the closing items get here — the
// captured row has to outlive the box that captured it.
static void quickMenuAct(QuickItem item, int index) {
  switch (item) {
    case QuickItem::FontSize:
      fontSize = static_cast<FontSize>((static_cast<int>(fontSize) + 1) % 3);
      break;
    case QuickItem::Typeface:
      editorFont = (editorFont == EditorFont::SANS) ? EditorFont::MONO
                                                    : EditorFont::SANS;
      break;
    case QuickItem::DarkMode:
      darkMode = !darkMode;
      break;
    case QuickItem::WritingMode:
      writingMode = static_cast<WritingMode>((static_cast<int>(writingMode) + 1) % 3);
      break;

    case QuickItem::Rename:
      if (currentState == UIState::TEXT_EDITOR) {
        openTitleEdit(editorGetCurrentTitle(), UIState::TEXT_EDITOR);
      } else {
        FileInfo* note = noteVisibleAt(index);
        if (note) openTitleEdit(note->title, UIState::FILE_BROWSER);
      }
      break;

    case QuickItem::Delete:
      // Hands straight over to the confirmation box — the quick menu never
      // destroys anything itself, so there is exactly one place that asks and
      // exactly one place that deletes.
      if (currentState == UIState::TEXT_EDITOR) {
        if (editorGetCurrentFile()[0] != '\0') {
          confirmOpen(confirmDialog, ConfirmKind::DeleteOpenNote, -1);
        }
      } else {
        confirmOpen(confirmDialog, ConfirmKind::DeleteNote, index);
      }
      break;

    default:
      break;
  }
}

static void settingsGotoTab(int tab) {
  settingsTab = ((tab % SETTINGS_TAB_COUNT) + SETTINGS_TAB_COUNT) % SETTINGS_TAB_COUNT;
  settingsSelection = settingsTabRow(settingsTab, 0);
}

static void dispatchEvent(const KeyEvent& event) {
  if (!event.pressed) return;

  lastKeyHid = event.keyCode;
  lastKeyMods = event.modifiers;

  switch (currentState) {
    case UIState::MAIN_MENU: {
      int menuCount = BASE_MENU_COUNT + otaAppCount;
      if (event.keyCode == HID_KEY_DOWN) {
        mainMenuSelection = (mainMenuSelection + 1) % menuCount;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        mainMenuSelection = (mainMenuSelection - 1 + menuCount) % menuCount;
        screenDirty = true;
      } else if (isCtrl(event.modifiers) && event.keyCode == HID_KEY_N) {
        // Ctrl+N renames the entry, echoing the browser's Ctrl+N for a note
        // title. Only the OTA rows have a name of their own to change.
        if (mainMenuSelection >= BASE_MENU_COUNT) {
          const int idx = mainMenuSelection - BASE_MENU_COUNT;
          if (idx < otaAppCount) {
            openTitleEdit(otaApps[idx].name, UIState::MAIN_MENU,
                          RenameTarget::OtaAppName, idx);
          }
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (mainMenuSelection == MENU_BROWSE) {
          refreshFileList();
          currentState = UIState::FILE_BROWSER;
          screenDirty = true;
        } else if (mainMenuSelection == MENU_NEW_NOTE) {
          createNewFile();
          openTitleEdit("Untitled", UIState::TEXT_EDITOR);
        } else if (mainMenuSelection == MENU_SETTINGS) {
          currentState = UIState::SETTINGS;
          screenDirty = true;
        } else if (mainMenuSelection == MENU_SYNC) {
          wifiSyncStart();
          currentState = UIState::WIFI_SYNC;
          screenDirty = true;
        } else if (mainMenuSelection == MENU_SYNC_AP) {
          wifiSyncStartAp();
          currentState = UIState::WIFI_SYNC;
          screenDirty = true;
        } else if (mainMenuSelection >= BASE_MENU_COUNT) {
          switchToOtaApp(mainMenuSelection - BASE_MENU_COUNT);
        }
      } else {
        // Digit shortcut: move the selector to that row and stop there. It
        // does NOT activate the row — the user still presses Enter. Several
        // of these entries are one-way (Sync brings up the radio, an OTA
        // entry reboots into another app), and on a screen that takes a
        // moment to redraw a mistyped digit must not be able to commit to one.
        const int row = menuDigit(event.keyCode, event.modifiers);
        if (row >= 1 && row <= menuCount) {
          mainMenuSelection = row - 1;
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::FILE_BROWSER: {
      int fc = noteVisibleCount();

      // The quick menu goes first. The two are never up together — choosing
      // Delete in the menu closes it and raises the box — so the order is for
      // the reader, not for correctness.
      if (quickMenu.open()) {
        const int raisedOn = quickMenu.index;
        const QuickResult r = quickKey(quickMenu, event.keyCode, event.modifiers);
        if (r.reply == QuickReply::Chosen) quickMenuAct(r.item, raisedOn);
        if (r.reply != QuickReply::Swallowed) screenDirty = true;
        break;
      }

      // The confirmation box is modal: while it is up nothing else on this
      // screen sees a key. It deletes the note it captured when it opened, not
      // whatever the selection happens to point at by then.
      if (confirmDialog.open()) {
        const ConfirmReply reply = confirmKey(confirmDialog, event.keyCode, event.modifiers);
        if (reply == ConfirmReply::Confirmed) {
          FileInfo* note = noteVisibleAt(confirmDialog.index);
          if (note) deleteFile(note->filename);
          noteClampSelection(&selectedFileIndex);
        }
        // A swallowed key changed nothing on screen, and a redraw costs a
        // panel refresh — so it does not get one.
        if (reply != ConfirmReply::Swallowed) screenDirty = true;
        break;
      }

      if (isCtrl(event.modifiers) && event.keyCode == HID_KEY_K) {
        // The same menu the select button raises on a hold. Redundant here —
        // Ctrl+N and Ctrl+D already do both of its actions — but the gesture
        // should mean the same thing on both screens.
        if (fc > 0) {
          quickOpen(quickMenu, QuickKind::Note, selectedFileIndex);
          screenDirty = true;
        }
      } else if (isCtrl(event.modifiers) && event.keyCode == HID_KEY_N) {
        FileInfo* note = noteVisibleAt(selectedFileIndex);
        if (note) openTitleEdit(note->title, UIState::FILE_BROWSER);
      } else if (isCtrl(event.modifiers) && event.keyCode == HID_KEY_D) {
        if (fc > 0) {
          confirmOpen(confirmDialog, ConfirmKind::DeleteNote, selectedFileIndex);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_DOWN && fc > 0) {
        selectedFileIndex = (selectedFileIndex + 1) % fc;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP && fc > 0) {
        selectedFileIndex = (selectedFileIndex - 1 + fc) % fc;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_ENTER && fc > 0) {
        FileInfo* note = noteVisibleAt(selectedFileIndex);
        if (note) loadFile(note->filename);
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_BACKSPACE) {
        if (noteFilterText()[0] != '\0') {
          noteFilterBackspace();
          noteClampSelection(&selectedFileIndex);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ESCAPE) {
        if (noteFilterText()[0] != '\0') {
          noteFilterClear();
          selectedFileIndex = 0;
          screenDirty = true;
        } else {
          currentState = UIState::MAIN_MENU;
          screenDirty = true;
        }
      } else if (!isCtrl(event.modifiers)) {
        uint32_t cps[2];
        const int produced = inputResolveText(event.keyCode, event.modifiers, cps);
        if (produced > 0) {
          for (int i = 0; i < produced; i++) noteFilterPushCodepoint(cps[i]);
          noteClampSelection(&selectedFileIndex);
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::TEXT_EDITOR:
      handleEditorKey(event.keyCode, event.modifiers);
      break;

    case UIState::RENAME_FILE:
      handleRenameKey(event.keyCode, event.modifiers);
      break;

    case UIState::SETTINGS: {
      // Two axes now: Up/Down walks the rows of the open tab, Left/Right walks
      // the tabs. That costs Left its old job of stepping a value backward,
      // which moves to Shift+Enter — the physical case has four direction
      // buttons and no modifier key, so the tabs have to be reachable from the
      // arrows or they are not reachable at all without a keyboard.
      const int rowCount = settingsTabRowCount(settingsTab);
      const SettingsPlace here = settingsPlaceOf(settingsSelection);
      const int pos = here.pos >= 0 ? here.pos : 0;

      if (event.keyCode == HID_KEY_DOWN) {
        settingsSelection = settingsTabRow(settingsTab, (pos + 1) % rowCount);
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        settingsSelection =
            settingsTabRow(settingsTab, (pos - 1 + rowCount) % rowCount);
        screenDirty = true;

      // Tabs: the arrows, the function keys the design draws on them, and Tab.
      } else if (event.keyCode == HID_KEY_RIGHT || event.keyCode == HID_KEY_TAB) {
        settingsGotoTab(settingsTab + 1);
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_LEFT) {
        settingsGotoTab(settingsTab - 1);
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_F1) {
        settingsGotoTab(TAB_SYSTEM);
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_F2) {
        settingsGotoTab(TAB_EDITOR);
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_F3) {
        settingsGotoTab(TAB_CONTROLS);
        screenDirty = true;

      // Enter changes the row's value; Shift+Enter walks it back.
      //
      // Forward and backward used to be two separate if-chains, one per key,
      // and a row added to only one of them would have cycled in one direction
      // and stuck in the other — the same failure mode as a parallel array.
      // One chain with a step cannot do that.
      } else if (event.keyCode == HID_KEY_ENTER) {
        const int step = isShift(event.modifiers) ? -1 : 1;
        auto cyc = [step](int v, int n) { return ((v + step) % n + n) % n; };

        if (settingsSelection == SET_ORIENTATION) {
          currentOrientation =
              static_cast<Orientation>(cyc(static_cast<int>(currentOrientation), 4));
        } else if (settingsSelection == SET_DARK_MODE) {
          darkMode = !darkMode;
        } else if (settingsSelection == SET_WRITING) {
          writingMode = static_cast<WritingMode>(cyc(static_cast<int>(writingMode), 3));
        } else if (settingsSelection == SET_FONT_SIZE) {
          fontSize = static_cast<FontSize>(cyc(static_cast<int>(fontSize), 3));
        } else if (settingsSelection == SET_EDITOR_FONT) {
          editorFont = (editorFont == EditorFont::SANS) ? EditorFont::MONO
                                                        : EditorFont::SANS;
        } else if (settingsSelection == SET_KEYBOARD) {
          keyboardLayout =
              static_cast<KeyboardLayout>(cyc(static_cast<int>(keyboardLayout), 3));
          inputClearDeadKey();   // don't carry an armed accent across the switch
        } else if (settingsSelection == SET_SLEEP) {
          sleepScreenMode =
              static_cast<SleepScreenMode>(cyc(static_cast<int>(sleepScreenMode), 3));
        } else if (settingsSelection == SET_NOTE_ORDER) {
          noteSort = static_cast<NoteSort>(cyc(static_cast<int>(noteSort), 4));
        } else if (settingsSelection == SET_BLUETOOTH) {
          currentState = UIState::BLUETOOTH_SETTINGS;
        } else if (settingsSelection == SET_PAIRED_KB) {
          pairedKeyboardSelection = 0;
          currentState = UIState::PAIRED_KEYBOARDS;
        } else if (settingsSelection == SET_DPAD) {
          dpadMode = (dpadMode == DpadMode::Fixed) ? DpadMode::Natural
                                                   : DpadMode::Fixed;
        }
        screenDirty = true;

      } else if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;

      } else {
        // Digit shortcut: park the selector on that row without changing its
        // value. Enter still does the change — here that matters twice over,
        // since these rows cycle a setting in place rather than opening
        // something, so a digit that also acted would silently rotate a value.
        //
        // The digits count rows WITHIN the open tab, which is what the screen
        // draws beside them. Tabs are what gave the eleventh row its number
        // back: no tab is longer than the digits.
        const int row = menuDigit(event.keyCode, event.modifiers);
        if (row >= 1 && row <= rowCount) {
          settingsSelection = settingsTabRow(settingsTab, row - 1);
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::BLUETOOTH_SETTINGS: {
      int deviceCount = getDiscoveredDeviceCount();

      // Ensure selection is within bounds
      if (bluetoothDeviceSelection >= deviceCount && deviceCount > 0) {
        bluetoothDeviceSelection = deviceCount - 1;
      } else if (deviceCount == 0) {
        bluetoothDeviceSelection = 0; // Reset to 0 when no devices
      }

      if (event.keyCode == HID_KEY_ESCAPE) {
        DBG_PRINTLN("[INPUT] BT: Escape pressed - returning to settings");
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_DOWN) {
        if (deviceCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection + 1) % deviceCount;
          DBG_PRINTF("[INPUT] BT: Down pressed - selection now %d/%d\n", bluetoothDeviceSelection, deviceCount);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_UP) {
        if (deviceCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection - 1 + deviceCount) % deviceCount;
          DBG_PRINTF("[INPUT] BT: Up pressed - selection now %d/%d\n", bluetoothDeviceSelection, deviceCount);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (deviceCount > 0 && !isDeviceScanning()) {
          // Connect to the selected device
          connectToDevice(bluetoothDeviceSelection);
        } else if (!isDeviceScanning()) {
          // No devices — start a new scan
          startDeviceScan();
        }
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_RIGHT) {
        // Right button = re-scan for devices
        if (!isDeviceScanning()) {
          startDeviceScan();
        }
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_LEFT) {
        if (isKeyboardConnected()) {
          disconnectCurrentDevice();
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::PAIRED_KEYBOARDS: {
      int count = getPairedKeyboardCount();

      if (confirmDialog.open()) {
        const ConfirmReply reply = confirmKey(confirmDialog, event.keyCode, event.modifiers);
        if (reply == ConfirmReply::Confirmed) {
          removePairedKeyboard(confirmDialog.index);
          const int newCount = getPairedKeyboardCount();
          if (pairedKeyboardSelection >= newCount)
            pairedKeyboardSelection = newCount > 0 ? newCount - 1 : 0;
        }
        if (reply != ConfirmReply::Swallowed) screenDirty = true;
        break;
      }

      if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_DOWN) {
        if (count > 0) {
          pairedKeyboardSelection = (pairedKeyboardSelection + 1) % count;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_UP) {
        if (count > 0) {
          pairedKeyboardSelection = (pairedKeyboardSelection - 1 + count) % count;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (count > 0) {
          connectToPairedKeyboard(pairedKeyboardSelection);
          currentState = UIState::SETTINGS;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_D) {
        if (count > 0) {
          confirmOpen(confirmDialog, ConfirmKind::ForgetKeyboard, pairedKeyboardSelection);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_LEFT) {
        // Disconnect if this keyboard is the currently connected one
        if (count > 0 && isKeyboardConnected()) {
          std::string addr, name; uint8_t addrType;
          getPairedKeyboard(pairedKeyboardSelection, addr, name, addrType);
          if (getCurrentDeviceAddress() == addr) {
            disconnectCurrentDevice();
            screenDirty = true;
          }
        }
      }
      break;
    }

    case UIState::WIFI_SYNC: {
      syncHandleKey(event.keyCode, event.modifiers);
      break;
    }

    default:
      break;
  }
}

int processAllInput() {
  int processedCount = 0;
  while (!isQueueEmpty()) {
    KeyEvent event = dequeueKeyEvent();
    dispatchEvent(event);
    processedCount++;
  }
  return processedCount;
}
