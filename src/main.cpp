#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>
#include <esp_pm.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_core_dump.h>
#include <Preferences.h>
#include <RecoveryBoot.h>
#include "sd_backup.h"

#include "config.h"
#include "ble_keyboard.h"
#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ui_renderer.h"
#include "wifi_sync.h"

// Enum for sleep reasons
enum class SleepReason {
  POWER_LONGPRESS,
  IDLE_TIMEOUT,
  MENU_ACTION
};

// Forward declarations
void renderSleepScreen();
void enterDeepSleep(SleepReason reason);

// External variables
extern bool autoReconnectEnabled;

// --- Hardware objects ---
HalDisplay display;
GfxRenderer renderer(display);
HalGPIO gpio;


// --- Persistent settings (NVS) ---
static Preferences uiPrefs;

// --- Reboot provenance -------------------------------------------------------
// esp_reset_reason() reports RST:SW for EVERY deliberate restart, so it cannot
// tell "the 5s BACK handler fired" apart from "the Escape Hatch sent us here".
// On a USB-locked device with no serial console that ambiguity is expensive, so
// each restart site stamps its own tag in NVS on the way out, and the next boot
// reads it back into lastRebootTag for the footer.
char lastRebootTag[24] = "";

void logRebootReason(const char* tag) {
  Preferences p;
  if (p.begin("diag", false)) {
    p.putString("why", tag);
    p.end();
  }
}

// --- Boot history -----------------------------------------------------------
// Reading the footer requires walking back through the Escape Hatch, whose "Boot
// Other Slot" calls esp_restart() — so by the time the screen is visible,
// esp_reset_reason() only ever reports SW and the original cause is gone. Fix:
// record each boot's reason in NVS BEFORE anything can restart us, and keep a
// short history.
//
// Each entry is <reason><mark>, where the mark is:
//   ?  this boot never got past the recovery hatch — checkBootCombo() diverted
//      it to the Escape Hatch, which means the Back+Up combo read as held
//   !  this boot survived the hatch and reached the app
//
// So "BRN? SW!" reads as: a brownout reboot that the combo then intercepted,
// followed by a deliberate restart that came up normally.
//
// This writes to NVS twice per boot. It is a debugging aid — drop it once the
// BLE connect crash is understood, rather than leaving it wearing the flash.
char bootHistory[64] = "";

static const char* reasonAbbrev() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "PWR";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PAN";
    case ESP_RST_INT_WDT:   return "IWD";
    case ESP_RST_TASK_WDT:  return "TWD";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DSL";
    case ESP_RST_BROWNOUT:  return "BRN";
    default:                return "UNK";
  }
}

// Called BEFORE checkBootCombo(). Appends "<reason>?" to the history.
static void bootHistoryAppend() {
  Preferences p;
  if (!p.begin("diag", false)) return;
  String h = p.getString("hist", "");
  h += reasonAbbrev();
  h += "? ";
  while (h.length() > 40) {           // keep only the recent tail
    int sp = h.indexOf(' ');
    if (sp < 0) break;
    h = h.substring(sp + 1);
  }
  p.putString("hist", h);
  snprintf(bootHistory, sizeof(bootHistory), "%s", h.c_str());
  p.end();
}

// Called AFTER checkBootCombo() returned — i.e. the hatch did NOT divert us.
// Flips this boot's trailing '?' to '!'.
static void bootHistoryConfirm() {
  Preferences p;
  if (!p.begin("diag", false)) return;
  String h = p.getString("hist", "");
  int q = h.lastIndexOf('?');
  if (q >= 0) {
    h.setCharAt(q, '!');
    p.putString("hist", h);
    snprintf(bootHistory, sizeof(bootHistory), "%s", h.c_str());
  }
  p.end();
}

// Breadcrumb in NVS. The RTC-memory version read back as 0 even when the code
// had clearly run, so trust the storage that demonstrably works instead.
char lastCrumb[16] = "";

void logCrumb(const char* tag) {
  Preferences p;
  if (p.begin("diag", false)) {
    p.putString("crumb", tag);
    p.end();
  }
}

// Pull the faulting task and program counter out of the core dump the panic
// handler wrote to flash. This is the closest thing to a backtrace available
// without a serial console: feed the PC to
//   xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/xteink_x4/firmware.elf <pc>
// (riscv32-esp-elf-addr2line on the C3) to get file:line.
// The image is erased after reading so a stale crash cannot masquerade as a new
// one.
char crashInfo[40] = "";

static void readCoreDump() {
  if (esp_core_dump_image_check() != ESP_OK) return;
  esp_core_dump_summary_t* sum =
      (esp_core_dump_summary_t*)malloc(sizeof(esp_core_dump_summary_t));
  if (!sum) return;
  if (esp_core_dump_get_summary(sum) == ESP_OK) {
    snprintf(crashInfo, sizeof(crashInfo), "%s@%08lx",
             sum->exc_task, (unsigned long)sum->exc_pc);
  }
  free(sum);
  esp_core_dump_image_erase();
}

static void readCrumb() {
  Preferences p;
  if (!p.begin("diag", false)) return;
  String c = p.getString("crumb", "");
  snprintf(lastCrumb, sizeof(lastCrumb), "%s", c.c_str());
  p.end();
}

static void readAndClearRebootReason() {
  Preferences p;
  if (!p.begin("diag", false)) return;
  String why = p.getString("why", "");
  if (why.length() > 0) {
    snprintf(lastRebootTag, sizeof(lastRebootTag), "%s", why.c_str());
    p.remove("why");   // one-shot: don't let a stale tag explain a later boot
  }
  p.end();
}

// --- Shared UI state ---
UIState currentState = UIState::MAIN_MENU;
int mainMenuSelection = 0;
int selectedFileIndex = 0;
int settingsSelection = 0;
int bluetoothDeviceSelection = 0;
int pairedKeyboardSelection = 0;
Orientation currentOrientation = Orientation::PORTRAIT;
int charsPerLine = 40;
bool screenDirty = true;

// Rename buffer
char renameBuffer[MAX_FILENAME_LEN] = "";
int renameBufferLen = 0;

// UI mode flags
bool darkMode = false;
bool cleanMode = false;
bool deleteConfirmPending = false;
WritingMode writingMode = WritingMode::NORMAL;
FontSize fontSize = FontSize::LARGE;
bool showWordCount = true;

// --- OTA App Detection ---
OtaAppEntry otaApps[MAX_OTA_APPS];
int otaAppCount = 0;

// Register this app's display name in shared NVS, keyed by OTA slot number.
static void registerOtaAppName(const char* name) {
  const esp_partition_t* self = esp_ota_get_running_partition();
  if (!self) return;
  int slot = self->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
  char key[8];
  snprintf(key, sizeof(key), "ota_%d", slot);
  Preferences prefs;
  prefs.begin("ota_names", false);
  prefs.putString(key, name);
  prefs.end();
  DBG_PRINTF("[OTA] Registered as \"%s\" in slot %d\n", name, slot);
}

// Scan all OTA partitions (except self), check for valid firmware, populate otaApps[].
static void detectOtaApps() {
  otaAppCount = 0;
  const esp_partition_t* running = esp_ota_get_running_partition();
  Preferences otaPrefs;
  otaPrefs.begin("ota_names", true);  // read-only

  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                    ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it != NULL && otaAppCount < MAX_OTA_APPS) {
    const esp_partition_t* part = esp_partition_get(it);
    if (part && part != running
        && part->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0
        && part->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_15) {

      esp_app_desc_t desc;
      if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        int slot = part->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
        char key[8];
        snprintf(key, sizeof(key), "ota_%d", slot);
        String nvsName = otaPrefs.getString(key, "");

        OtaAppEntry& entry = otaApps[otaAppCount];
        if (nvsName.length() > 0) {
          strncpy(entry.name, nvsName.c_str(), sizeof(entry.name) - 1);
        } else {
          snprintf(entry.name, sizeof(entry.name), "OTA Slot %d", slot);
        }
        entry.name[sizeof(entry.name) - 1] = '\0';
        entry.partitionSubtype = part->subtype;
        otaAppCount++;
      }
    }
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  otaPrefs.end();
  DBG_PRINTF("[OTA] Detected %d additional app(s)\n", otaAppCount);
}

// Switch to another OTA app by index into otaApps[]. Non-static so input_handler can call it.
void switchToOtaApp(int index) {
  if (index < 0 || index >= otaAppCount) return;
  int subtype = otaApps[index].partitionSubtype;
  const esp_partition_t* target = esp_partition_find_first(
      ESP_PARTITION_TYPE_APP,
      static_cast<esp_partition_subtype_t>(subtype), NULL);
  if (!target) {
    DBG_PRINTF("[OTA] Partition subtype %d not found!\n", subtype);
    return;
  }
  DBG_PRINTF("[OTA] Switching to \"%s\" (subtype %d)...\n", otaApps[index].name, subtype);
  esp_ota_set_boot_partition(target);
  logRebootReason("OTA_SWITCH");
  esp_restart();
}

// --- Screen update ---
static void updateScreen() {
  if (!screenDirty) return;
  screenDirty = false;

  // Apply orientation
  static Orientation lastOrientation = Orientation::PORTRAIT;
  if (currentOrientation != lastOrientation) {
    GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
    switch (currentOrientation) {
      case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
      case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
      case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
      case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
    }
    renderer.setOrientation(gfxOrient);
    lastOrientation = currentOrientation;
  }

  // Auto-compute chars per line from font metrics so text always fills the screen
  {
    int sw = renderer.getScreenWidth();
    int textAreaWidth = sw - 20;  // 10px margins each side
    int avgCharW = renderer.getTextAdvanceX(editorFontId(fontSize), "abcdefghijklmnopqrstuvwxyz") / 26;
    if (avgCharW > 0) charsPerLine = textAreaWidth / avgCharW;
  }
  editorSetCharsPerLine(charsPerLine);

  switch (currentState) {
    case UIState::MAIN_MENU:         drawMainMenu(renderer, gpio); break;
    case UIState::FILE_BROWSER:      drawFileBrowser(renderer, gpio); break;
    case UIState::TEXT_EDITOR:       drawTextEditor(renderer, gpio); break;
    case UIState::RENAME_FILE:       drawRenameScreen(renderer, gpio); break;
    case UIState::SETTINGS:          drawSettingsMenu(renderer, gpio); break;
    case UIState::BLUETOOTH_SETTINGS: drawBluetoothSettings(renderer, gpio); break;
    case UIState::PAIRED_KEYBOARDS:   drawPairedKeyboardsMenu(renderer, gpio); break;
    case UIState::WIFI_SYNC:          drawSyncScreen(renderer, gpio); break;
    default: break;
  }
}

void setup() {
  // ---------------------------------------------------------------------------
  // Recovery hatch — MUST stay the first statement in setup().
  //
  // The stock second-stage bootloader cannot read buttons, so a "hold a combo at
  // reset to escape" check can only be honoured by the firmware that boots. Held
  // at reset, Back + Up either flashes /update.bin from the SD card into the
  // next OTA slot, or — with no update file present — repoints otadata at ota_0
  // and reboots into the recovery firmware (Escape Hatch) living there.
  //
  // On a USB-locked X4 this is the ONLY way back out of this firmware. Do not
  // move it below display.begin(), and do not make it conditional.
  //
  // Safe to call unconditionally: it returns immediately unless the combo is
  // held, ota_0 holds a valid app image, and we are not already running from
  // ota_0. When it acts, it reboots and never returns.
  // ---------------------------------------------------------------------------
  // Record this boot's reset cause BEFORE the hatch can restart us and erase it.
  bootHistoryAppend();

  {
    freeink::recovery::SdUpdateOptions recovery;
    recovery.path = "/update.bin";
    recovery.renameOnSuccess = true;  // so holding the combo through the
                                      // post-flash reset can't reflash the
                                      // same image in a loop
    freeink::recovery::checkBootCombo(recovery);
  }

  // If the bootloader has rollback enabled, an image flashed into the other OTA
  // slot boots as PENDING_VERIFY: any reset afterwards sends the device back to
  // the previous slot — here, the Escape Hatch. That matches exactly what we see
  // (device lands in the hatch with no recovery-combo mark in the history), so
  // declare this image good as soon as we are running. Harmless no-op when
  // rollback is not enabled.
  esp_ota_mark_app_valid_cancel_rollback();

  bootHistoryConfirm();
  readCrumb();
  readCoreDump();
  readAndClearRebootReason();

  DBG_INIT();
  DBG_PRINTLN("MicroSlate starting...");

  setCpuFrequencyMhz(80);

  gpio.begin();
  display.begin();

  renderer.setFadingFix(true);  // Power down display analog circuits after each refresh — reduces idle drain
  rendererSetup(renderer);

  // Load persisted UI settings from NVS early so startup screen uses saved orientation
  uiPrefs.begin("ui_prefs", false);
  currentOrientation = static_cast<Orientation>(uiPrefs.getUChar("orient", 0));
  darkMode = uiPrefs.getBool("darkMode", false);
  writingMode = static_cast<WritingMode>(uiPrefs.getUChar("writeMode", 0));
  fontSize = static_cast<FontSize>(uiPrefs.getUChar("fontSize", 2));
  showWordCount = uiPrefs.getBool("showWC", true);

  // Apply saved orientation
  {
    GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
    switch (currentOrientation) {
      case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
      case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
      case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
      case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
    }
    renderer.setOrientation(gfxOrient);
  }

  editorInit();
  inputSetup();
  fileManagerSetup();

  // Restore UI prefs from SD backup if NVS was wiped by a firmware flash
  if (!uiPrefs.isKey("orient")) {
    static char uiBuf[128];
    if (sdReadFile("/microslate/ui_prefs.json", uiBuf, sizeof(uiBuf))) {
      int o  = jsonGetInt(uiBuf, "orient");
      int d  = jsonGetInt(uiBuf, "dark");
      int wm = jsonGetInt(uiBuf, "writeMode");
      int fs = jsonGetInt(uiBuf, "fontSize");
      int wc = jsonGetInt(uiBuf, "showWC");
      if (o  >= 0) { uiPrefs.putUChar("orient",    (uint8_t)o);  currentOrientation = static_cast<Orientation>(o); }
      if (d  >= 0) { uiPrefs.putBool("darkMode",   d != 0);      darkMode           = (d != 0); }
      if (wm >= 0) { uiPrefs.putUChar("writeMode", (uint8_t)wm); writingMode        = static_cast<WritingMode>(wm); }
      if (fs >= 0) { uiPrefs.putUChar("fontSize",  (uint8_t)fs); fontSize           = static_cast<FontSize>(fs); }
      if (wc >= 0) { uiPrefs.putBool("showWC",     wc != 0);     showWordCount      = (wc != 0); }
      // Re-apply orientation in case it changed
      GfxRenderer::Orientation gfxOrient = GfxRenderer::Portrait;
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
        case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
        case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
        case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
      }
      renderer.setOrientation(gfxOrient);
      DBG_PRINTLN("UI prefs restored from SD backup");
    }
  }

  bleSetup();

  // Enable automatic light sleep between loop iterations.
  // CONFIG_PM_ENABLE and CONFIG_FREERTOS_USE_TICKLESS_IDLE are compiled into
  // ESP-IDF via sdkconfig.defaults (framework = arduino, espidf). BLE modem
  // sleep keeps the radio alive across sleep/wake cycles.
  esp_pm_config_esp32c3_t pm_config = {
    .max_freq_mhz = 80,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  DBG_PRINTF("PM configure: %s\n", esp_err_to_name(pm_err));

  // Initialize auto-reconnect to enabled by default
  autoReconnectEnabled = true;

  // Register this app's name in shared NVS and detect other OTA apps
  registerOtaAppName("MicroSlate");
  detectOtaApps();

  DBG_PRINTLN("MicroSlate ready.");

  // The display needs one FULL_REFRESH after power-on to initialize its analog
  // circuits before FAST_REFRESH will work.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  screenDirty = true;
}

// Enter deep sleep - matches crosspoint pattern
void enterDeepSleep(SleepReason reason) {
  DBG_PRINTLN("Entering deep sleep...");
  
  // Render the sleep screen before entering deep sleep
  renderSleepScreen();

  // Save any unsaved work
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }

  display.deepSleep();     // Power down display first
  gpio.startDeepSleep();   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
static void processPhysicalButtons() {
  static bool btnUpLast = false;
  static bool btnDownLast = false;
  static bool btnLeftLast = false;
  static bool btnRightLast = false;
  static bool btnConfirmLast = false;
  static bool btnBackLast = false;

  // Use isPressed() — persistent debounced state.  With one-shot scanning
  // (radio quiet during navigation), InputManager debounce works reliably.
  bool btnUp      = gpio.isPressed(HalGPIO::BTN_UP);
  bool btnDown    = gpio.isPressed(HalGPIO::BTN_DOWN);
  bool btnLeft    = gpio.isPressed(HalGPIO::BTN_LEFT);
  bool btnRight   = gpio.isPressed(HalGPIO::BTN_RIGHT);
  bool btnConfirm = gpio.isPressed(HalGPIO::BTN_CONFIRM);
  bool btnBack    = gpio.isPressed(HalGPIO::BTN_BACK);

  // Power button state machine for proper long/short press handling
  static bool powerHeld = false;
  static unsigned long powerPressStart = 0;
  static bool sleepTriggered = false;

  bool btnPower = gpio.isPressed(HalGPIO::BTN_POWER);

  if (btnPower && !powerHeld) {
    // Button just pressed
    powerHeld = true;
    sleepTriggered = false;
    powerPressStart = millis();
  }

  if (btnPower && powerHeld && !sleepTriggered) {
    if (millis() - powerPressStart > 3000) {
      sleepTriggered = true;
      enterDeepSleep(SleepReason::POWER_LONGPRESS);
      return; // Exit early to prevent further processing
    }
  }

  if (!btnPower && powerHeld) {
    // Button released
    unsigned long duration = millis() - powerPressStart;
    powerHeld = false;

    if (!sleepTriggered && duration > 50 && duration < 1000) {
      // Short press - go to main menu (except when already there)
      if (currentState != UIState::MAIN_MENU) {
        if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
          saveCurrentFile();
        }
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
    }
  }

  // Back button long-press for restart
  static bool backHeld = false;
  static unsigned long backPressStart = 0;
  static bool restartTriggered = false;

  if (btnBack && !backHeld) {
    backHeld = true;
    restartTriggered = false;
    backPressStart = millis();
  }

  if (btnBack && backHeld && !restartTriggered) {
    if (millis() - backPressStart > 5000) {
      restartTriggered = true;
      DBG_PRINTLN("BACK held for 5s — restarting device...");
      if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
        saveCurrentFile();
      }
      logRebootReason("BACK_5S");
      delay(100);
      ESP.restart();
    }
  }

  if (!btnBack && backHeld) {
    backHeld = false;
  }

  // Map physical buttons to HID key codes based on current UI state
  switch (currentState) {
    case UIState::MAIN_MENU:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      break;

    case UIState::FILE_BROWSER:
      if (((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::TEXT_EDITOR: {
      // Key repeat state for held navigation/backspace keys
      static uint8_t repeatKey = 0;
      static unsigned long repeatStart = 0;
      static unsigned long lastRepeat = 0;
      const unsigned long REPEAT_DELAY = 400;
      const unsigned long REPEAT_RATE  = 80;

      auto fireKey = [](uint8_t k) {
        enqueueKeyEvent(k, 0, true);
        enqueueKeyEvent(k, 0, false);
      };

      // Map currently held button to HID key (0 = none)
      uint8_t heldKey = 0;
      if      (btnUp)    heldKey = HID_KEY_UP;
      else if (btnDown)  heldKey = HID_KEY_DOWN;
      else if (btnLeft)  heldKey = HID_KEY_LEFT;
      else if (btnRight) heldKey = HID_KEY_RIGHT;

      if (heldKey != repeatKey) {
        // Key changed — fire immediately on press
        if (heldKey != 0) fireKey(heldKey);
        repeatKey   = heldKey;
        repeatStart = millis();
        lastRepeat  = millis();
      } else if (heldKey != 0) {
        unsigned long now = millis();
        if (now - repeatStart > REPEAT_DELAY && now - lastRepeat > REPEAT_RATE) {
          fireKey(heldKey);
          lastRepeat = now;
        }
      }

      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        if (editorHasUnsavedChanges()) saveCurrentFile();
        currentState = UIState::FILE_BROWSER;
        screenDirty = true;
      }
      break;
    }

    case UIState::RENAME_FILE:
    case UIState::NEW_FILE:
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::BLUETOOTH_SETTINGS:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);  // Scan
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (btnLeft && !btnLeftLast) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);   // Disconnect
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::PAIRED_KEYBOARDS:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::WIFI_SYNC:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::SETTINGS:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    default:
      break;
  }

  // Update last state
  btnUpLast = btnUp;
  btnDownLast = btnDown;
  btnLeftLast = btnLeft;
  btnRightLast = btnRight;
  btnConfirmLast = btnConfirm;
  btnBackLast = btnBack;
}

// Global variable for activity tracking
static unsigned long lastActivityTime = 0;
const unsigned long IDLE_TIMEOUT = 5UL * 60UL * 1000UL; // 5 minutes

void registerActivity() {
  lastActivityTime = millis();
}

// Function to render the sleep screen
void renderSleepScreen() {
  renderer.clearScreen();
  
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  
  // Title: "MicroSlate"
  const char* title = "MicroSlate";
  int titleWidth = renderer.getTextAdvanceX(FONT_BODY, title);
  int titleX = (sw - titleWidth) / 2;
  int titleY = sh * 0.35; // 35% down the screen (moved up)
  renderer.drawText(FONT_BODY, titleX, titleY, title, true, EpdFontFamily::BOLD);
  
  // Subtitle: "Asleep"
  const char* subtitle = "Asleep";
  int subTitleWidth = renderer.getTextAdvanceX(FONT_UI, subtitle);
  int subTitleX = (sw - subTitleWidth) / 2;
  int subTitleY = sh * 0.48; // 48% down the screen (moved up)
  renderer.drawText(FONT_UI, subTitleX, subTitleY, subtitle, true);
  
  // Footer: "Hold Power to wake"
  const char* footer = "Hold Power to wake";
  int footerWidth = renderer.getTextAdvanceX(FONT_SMALL, footer);
  int footerX = (sw - footerWidth) / 2;
  int footerY = sh * 0.75; // 75% down the screen (moved up from bottom)
  renderer.drawText(FONT_SMALL, footerX, footerY, footer);
  
  // Perform a full display refresh to ensure the sleep screen is visible
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  
  // Small delay to ensure the display update is complete
  delay(500);
}

void loop() {
  // --- GPIO first: always poll buttons before anything else ---
  gpio.update();

  // Control auto-reconnect based on UI state
  static UIState lastState = UIState::MAIN_MENU;
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    autoReconnectEnabled = false;
    // On first entry to BT settings, do a one-shot scan
    if (lastState != UIState::BLUETOOTH_SETTINGS) {
      cancelPendingConnection();
      startDeviceScan();  // One-shot 5s scan, radio goes quiet after
    }
  } else {
    autoReconnectEnabled = true;
    if (lastState == UIState::BLUETOOTH_SETTINGS && isDeviceScanning()) {
      stopDeviceScan();
    }
  }
  lastState = currentState;

  // Process BLE (connection handling, scan completion detection)
  bleLoop();

  // Process WiFi sync HTTP clients when active
  if (isWifiSyncActive()) wifiSyncLoop();

  // CRITICAL: Process buttons BEFORE checking wasAnyPressed() to avoid consuming button states
  processPhysicalButtons();
  int inputEventsProcessed = processAllInput(); // Assuming this returns number of events processed

  // Register activity AFTER button processing (don't consume button states prematurely)
  static unsigned long lastInputTime = 0;
  bool hadActivity = gpio.wasAnyPressed() || inputEventsProcessed > 0;
  if (hadActivity) {
    registerActivity();
    lastInputTime = millis();
  }

  // Auto-save: hybrid idle + hard cap for crash protection.
  // - Saves after 10s of no keystrokes (catches natural pauses between sentences)
  // - Hard cap every 2min during continuous typing (never lose more than 2min of work)
  static unsigned long lastAutoSaveMs = 0;
  if (currentState == UIState::TEXT_EDITOR
      && editorHasUnsavedChanges()
      && editorGetCurrentFile()[0] != '\0') {
    unsigned long now = millis();
    bool idleTrigger = (now - lastInputTime) > AUTO_SAVE_IDLE_MS
                    && (now - lastAutoSaveMs) > AUTO_SAVE_IDLE_MS;
    bool capTrigger  = (now - lastAutoSaveMs) > AUTO_SAVE_MAX_MS;
    if (idleTrigger || capTrigger) {
      lastAutoSaveMs = now;
      saveCurrentFile(false);  // Skip refreshFileList — file list unchanged by content update
    }
  }

  // Periodically refresh sync screen to show status changes (every 2s)
  if (currentState == UIState::WIFI_SYNC) {
    static unsigned long lastSyncRefresh = 0;
    if (millis() - lastSyncRefresh > 2000) {
      screenDirty = true;
      lastSyncRefresh = millis();
    }
  }

  // Poll display refresh — non-blocking check of BUSY pin
  if (renderer.isRefreshing()) {
    renderer.pollRefresh();
  }

  // Don't start a new screen update while display is still refreshing
  if (screenDirty && !renderer.isRefreshing()) {
    updateScreen();
  }

  // Persist UI settings to NVS when they change (NVS write only on change, not every loop)
  static Orientation lastSavedOrientation = currentOrientation;
  static bool lastSavedDarkMode = darkMode;
  static WritingMode lastSavedWritingMode = writingMode;
  static FontSize lastSavedFontSize = fontSize;
  static bool lastSavedShowWordCount = showWordCount;
  if (currentOrientation != lastSavedOrientation || darkMode != lastSavedDarkMode
      || writingMode != lastSavedWritingMode || fontSize != lastSavedFontSize
      || showWordCount != lastSavedShowWordCount) {
    uiPrefs.putUChar("orient", static_cast<uint8_t>(currentOrientation));
    uiPrefs.putBool("darkMode", darkMode);
    uiPrefs.putUChar("writeMode", static_cast<uint8_t>(writingMode));
    uiPrefs.putUChar("fontSize", static_cast<uint8_t>(fontSize));
    uiPrefs.putBool("showWC", showWordCount);
    lastSavedOrientation = currentOrientation;
    lastSavedDarkMode = darkMode;
    lastSavedWritingMode = writingMode;
    lastSavedFontSize = fontSize;
    lastSavedShowWordCount = showWordCount;
    // Keep SD backup in sync so settings survive a firmware flash
    static char uiBuf[128];
    snprintf(uiBuf, sizeof(uiBuf),
             "{\"orient\":%d,\"dark\":%d,\"writeMode\":%d,\"fontSize\":%d,\"showWC\":%d}",
             (int)currentOrientation, darkMode ? 1 : 0,
             (int)writingMode, (int)fontSize, showWordCount ? 1 : 0);
    if (!SdMan.exists("/microslate")) SdMan.mkdir("/microslate");
    sdWriteFile("/microslate/ui_prefs.json", uiBuf);
  }

  // Check for idle timeout (skip while WiFi sync is active)
  if (!isWifiSyncActive() && millis() - lastActivityTime > IDLE_TIMEOUT) {
    enterDeepSleep(SleepReason::IDLE_TIMEOUT);
  }

  // Adaptive delay with recently-active window for button responsiveness.
  // BLE keystrokes wake from light sleep via modem interrupt (delay value irrelevant).
  // Physical buttons are polled, so the idle delay must be short enough to catch a
  // quick tap (~80-150ms). 50ms idle guarantees 1-2 samples per press.
  // Stay at fast polling for 2s after any activity for snappy consecutive presses.
  static constexpr unsigned long ACTIVE_WINDOW_MS = 2000;
  bool recentlyActive = (millis() - lastInputTime) < ACTIVE_WINDOW_MS;
  delay((hadActivity || screenDirty || recentlyActive) ? 10 : 50);
}
