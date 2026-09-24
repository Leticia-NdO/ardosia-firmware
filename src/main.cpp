#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>
#include <esp_pm.h>
#include <driver/adc.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <Preferences.h>
#include <RecoveryBoot.h>
#include "sd_backup.h"
#include "dpad.h"
#include "confirm.h"
#include "quickmenu.h"

#include "config.h"
#include "ble_keyboard.h"
#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ui_renderer.h"
#include "sleep_screen.h"
#include "wifi_sync.h"

// --- Power button timing -----------------------------------------------------
// Hold-to-sleep was 3000ms here; crosspoint-reader uses 400ms
// (CrossPointSettings::getPowerButtonDuration) and that is what the hardware
// feels like it wants. Original value noted in case 400 turns out too twitchy.
//
// Shortening it is NOT just a smaller number. Two guards have to come with it,
// both of which crosspoint carries:
//
//  1. powerReleasedSinceWake — you wake the device by PRESSING power, and on a
//     deep-sleep wake the chip resets with the button still down. At 3000ms you
//     would have let go long before; at 400ms the device would fall straight
//     back asleep in your hand. Sleep is armed only after a release is seen.
//  2. allowSleepAt — a grace window after boot, so the first refresh finishes
//     before any hold can count.
static constexpr unsigned long POWER_SLEEP_HOLD_MS = 400;   // was 3000

// How long select has to be held to raise the quick menu. Between the power
// button's 400ms and the Back button's 5s, and on the short side on purpose:
// the tap it delays is the one that opens a note or types a newline.
static constexpr unsigned long QUICK_MENU_HOLD_MS = 500;
static constexpr unsigned long POWER_SHORT_PRESS_MIN_MS = 50;
static constexpr unsigned long POWER_WAKE_GRACE_MS = 2000;
static unsigned long allowSleepAt = 0;

// Enum for sleep reasons
enum class SleepReason {
  POWER_LONGPRESS,
  IDLE_TIMEOUT,
  MENU_ACTION
};

// Forward declarations
void drawStatusPopup(const char* msg);
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

// --- One-shot cleanup of the old diagnostics ---------------------------------
// The temporary crash instrumentation (boot history, connect breadcrumb, reboot
// tag) kept its state in the "diag" NVS namespace, written on every boot. The
// code is gone; this drops the keys it left behind so the flash is not carrying
// dead state. Self-limiting: once the keys are removed isKey() is false and no
// further write ever happens. Safe to delete after one boot of this firmware.
static void dropLegacyDiagKeys() {
  Preferences p;
  if (!p.begin("diag", true)) return;            // namespace never existed
  const bool stale = p.isKey("hist") || p.isKey("crumb") || p.isKey("why");
  p.end();
  if (!stale) return;
  if (p.begin("diag", false)) {
    p.remove("hist");
    p.remove("crumb");
    p.remove("why");
    p.end();
  }
}

// --- Shared UI state ---
UIState currentState = UIState::MAIN_MENU;
int mainMenuSelection = 0;
int selectedFileIndex = 0;
// Derived from the tab table rather than hard-coded to 0: the invariant the
// Settings screen relies on is that settingsSelection is always a row of
// settingsTab, and a 0 here would only happen to satisfy it for as long as the
// first row of the first tab stayed SET_DARK_MODE.
int settingsSelection = settingsTabRow(TAB_SYSTEM, 0);
// Which Settings tab is open. RAM only, on purpose: it is worth keeping across
// a visit and not worth a flash write, and a device that always opens Settings
// on the same tab is easier to predict than one that remembers.
int settingsTab = TAB_SYSTEM;
NoteSort noteSort = NoteSort::ALPHA_ASC;
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
// The confirmation box for destructive actions. Lives here with the rest of
// the UI state; the screens read it, input_handler drives it, confirm.cpp holds
// the decisions and is tested on the host.
ConfirmDialog confirmDialog;

// The quick menu, raised by holding select (or Ctrl+K). Same arrangement as the
// dialog above: state here, decisions in quickmenu.cpp where they are testable.
QuickMenu quickMenu;
WritingMode writingMode = WritingMode::NORMAL;
FontSize fontSize = FontSize::LARGE;
EditorFont editorFont = EditorFont::SANS;
DpadMode dpadMode = DpadMode::Fixed;
bool showWordCount = true;
// Defaults to ABNT2: this fork is written in Portuguese and pairs with a
// Brazilian keyboard. Switchable in Settings (US / US-Intl / ABNT2).
KeyboardLayout keyboardLayout = KeyboardLayout::ABNT2;
SleepScreenMode sleepScreenMode = SleepScreenMode::TEXT;
SleepBrightness sleepBrightness = SleepBrightness::NORMAL;

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

// Rename another app's menu entry. The name lives in the same shared NVS
// namespace registerOtaAppName() writes to, keyed by the slot number — so the
// other app reads it too, and a name set here survives reflashing this one.
// Passing an empty name clears the override and the generic "OTA Slot N"
// returns, which is the way back from a name you regret.
bool renameOtaApp(int index, const char* name) {
  if (index < 0 || index >= otaAppCount || name == nullptr) return false;
  const int slot = otaApps[index].partitionSubtype - ESP_PARTITION_SUBTYPE_APP_OTA_0;
  if (slot < 0 || slot > 15) return false;

  char key[8];
  snprintf(key, sizeof(key), "ota_%d", slot);
  Preferences prefs;
  prefs.begin("ota_names", false);
  if (name[0] == '\0') prefs.remove(key);
  else                  prefs.putString(key, name);
  prefs.end();

  if (name[0] == '\0') {
    snprintf(otaApps[index].name, sizeof(otaApps[index].name), "OTA Slot %d", slot);
  } else {
    strncpy(otaApps[index].name, name, sizeof(otaApps[index].name) - 1);
    otaApps[index].name[sizeof(otaApps[index].name) - 1] = '\0';
  }
  return true;
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
    int avgCharW = renderer.getTextAdvanceX(editorFontId(fontSize, editorFont), "abcdefghijklmnopqrstuvwxyz") / 26;
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
  // the previous slot — here, the Escape Hatch. That is exactly what this device
  // did before this call existed, so declare the image good as soon as we are
  // running. Harmless no-op when rollback is not enabled.
  esp_ota_mark_app_valid_cancel_rollback();

  dropLegacyDiagKeys();

  DBG_INIT();
  DBG_PRINTLN("Ardosia starting...");

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
  editorFont = static_cast<EditorFont>(uiPrefs.getUChar("editFont", 0));
  if (static_cast<int>(editorFont) > 1) editorFont = EditorFont::SANS;
  dpadMode = static_cast<DpadMode>(uiPrefs.getUChar("dpad", 0));
  if (static_cast<int>(dpadMode) > 1) dpadMode = DpadMode::Fixed;
  showWordCount = uiPrefs.getBool("showWC", true);
  keyboardLayout = static_cast<KeyboardLayout>(
      uiPrefs.getUChar("kbLayout", static_cast<uint8_t>(KeyboardLayout::ABNT2)));
  if (static_cast<int>(keyboardLayout) > 2) keyboardLayout = KeyboardLayout::ABNT2;
  sleepScreenMode = static_cast<SleepScreenMode>(uiPrefs.getUChar("sleepScr", 0));
  if (static_cast<int>(sleepScreenMode) > 2) sleepScreenMode = SleepScreenMode::TEXT;
  sleepBrightness = static_cast<SleepBrightness>(uiPrefs.getUChar("sleepLight", 0));
  if (static_cast<int>(sleepBrightness) > 2) sleepBrightness = SleepBrightness::NORMAL;
  noteSort = static_cast<NoteSort>(uiPrefs.getUChar("noteSort", 0));
  if (static_cast<int>(noteSort) > 3) noteSort = NoteSort::ALPHA_ASC;

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

  // First thing the panel can show. BLE and the SD mount are the slow part
  // of wake; without this the glass stays on the sleep image until the menu.
  drawStatusPopup("Waking up...");

  editorInit();
  inputSetup();
  fileManagerSetup();

  // Must run after the card is mounted (fileManagerSetup) and before anything
  // reads a backup: the UI prefs restore below, bleSetup(), and Wi-Fi sync later.
  sdMigrateLegacyBackupDir();

  // Restore UI prefs from SD backup if NVS was wiped by a firmware flash
  if (!uiPrefs.isKey("orient")) {
    static char uiBuf[256];
    if (sdReadFile("/ardosia/ui_prefs.json", uiBuf, sizeof(uiBuf))) {
      int o  = jsonGetInt(uiBuf, "orient");
      int d  = jsonGetInt(uiBuf, "dark");
      int wm = jsonGetInt(uiBuf, "writeMode");
      int fs = jsonGetInt(uiBuf, "fontSize");
      int wc = jsonGetInt(uiBuf, "showWC");
      int kb = jsonGetInt(uiBuf, "kbLayout");
      int ss = jsonGetInt(uiBuf, "sleepScr");
      int sl = jsonGetInt(uiBuf, "sleepLight");
      int ns = jsonGetInt(uiBuf, "noteSort");
      int ef = jsonGetInt(uiBuf, "editFont");
      int dp = jsonGetInt(uiBuf, "dpad");
      if (o  >= 0) { uiPrefs.putUChar("orient",    (uint8_t)o);  currentOrientation = static_cast<Orientation>(o); }
      if (d  >= 0) { uiPrefs.putBool("darkMode",   d != 0);      darkMode           = (d != 0); }
      if (wm >= 0) { uiPrefs.putUChar("writeMode", (uint8_t)wm); writingMode        = static_cast<WritingMode>(wm); }
      if (fs >= 0) { uiPrefs.putUChar("fontSize",  (uint8_t)fs); fontSize           = static_cast<FontSize>(fs); }
      if (wc >= 0) { uiPrefs.putBool("showWC",     wc != 0);     showWordCount      = (wc != 0); }
      if (kb >= 0) { uiPrefs.putUChar("kbLayout",  (uint8_t)kb); keyboardLayout     = static_cast<KeyboardLayout>(kb); }
      if (ss >= 0) { uiPrefs.putUChar("sleepScr",  (uint8_t)ss); sleepScreenMode    = static_cast<SleepScreenMode>(ss); }
      if (sl >= 0) { uiPrefs.putUChar("sleepLight",(uint8_t)sl); sleepBrightness    = static_cast<SleepBrightness>(sl); }
      if (ns >= 0 && ns <= 3) {
        uiPrefs.putUChar("noteSort", (uint8_t)ns);
        noteSort = static_cast<NoteSort>(ns);
      }
      if (ef >= 0 && ef <= 1) {
        uiPrefs.putUChar("editFont", (uint8_t)ef);
        editorFont = static_cast<EditorFont>(ef);
      }
      if (dp >= 0 && dp <= 1) {
        uiPrefs.putUChar("dpad", (uint8_t)dp);
        dpadMode = static_cast<DpadMode>(dp);
      }
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
  registerOtaAppName("Ardosia");
  detectOtaApps();

  DBG_PRINTLN("Ardosia ready.");

  // The display needs one FULL_REFRESH after power-on to initialize its analog
  // circuits before FAST_REFRESH will work.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  // Don't let a hold that started before boot finished count as a sleep gesture.
  allowSleepAt = millis() + POWER_WAKE_GRACE_MS;

  screenDirty = true;
}

// Enter deep sleep - matches crosspoint pattern
void enterDeepSleep(SleepReason reason) {
  DBG_PRINTLN("Entering deep sleep...");
  
  // Save any unsaved work FIRST. Rendering now touches the SD card to load a
  // wallpaper, so it is no longer the trivially safe step it used to be, and
  // unsaved text must not depend on it returning.
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }

  renderSleepScreen();

  display.deepSleep();     // Power down display first
  gpio.startDeepSleep();   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
// --- Button ladder diagnostic — TEMPORARY -----------------------------------
// The buttons are an ADC ladder, not digital lines (InputManager.cpp:57). The
// channel that carries Up and Down decodes ANY value in 1121..3800 as Up — some
// 65% of the ADC's range — because the ladder only has to tell Up (~2242) from
// Down (~5) and from rest (~4095). So every transient that pin produces reads
// as Up, and never as Down, which matches the reported symptom exactly.
//
// That is a mechanism, not a proof, and this repository has a history of the
// obvious hypothesis being wrong. So: record what channel 2 actually does,
// rather than guess. The readout appears in the Settings footer.
//
// Delete this block, buttonDiagText(), its call in drawSettingsMenu and the
// counter bump below to remove the instrument.
static constexpr bool BUTTON_DIAG = true;

// ROUND 2. The first round refuted the obvious idea: at rest the channel sits
// on 4095 and never wanders into the Up window, so nothing is drifting there on
// its own. What the readings did show is that Down reads "4, 5, 6 or 4095"
// while held — the contact is bouncing — and that is the lead:
//
//   to get from 5 (Down) back to 4095 (rest) the voltage has to cross
//   1121..3800, and that IS the Up window.
//
// So every bounce of Down sweeps through Up, and boot does the same as the pin
// charges from 0 to 4095. That would make the phantom Up not spontaneous at all
// but a shadow of a Down — which fits "I could not catch it jumping by itself".
//
// This round records what the channel was doing AT the moment an Up event was
// enqueued, and how low it had been just before. A real Up press should fire
// around 2248 with nothing low behind it; a sweep should fire somewhere else
// entirely with a ~5 in its recent past.
static int adcCh2Now = -1;
static uint16_t upEventsSent = 0;

static constexpr int DIAG_RING = 16;   // ~160 ms of history at the active cadence
static int diagRing[DIAG_RING];
static int diagRingPos = 0;
static bool diagRingFilled = false;

static int upEventAdc = -1;      // channel value when the last Up event fired
static int upEventPreMin = -1;   // lowest value in the samples just before it

// Has an Up event ever fired with a Down in its recent past? That is the
// signature of the sweep: the voltage climbing from ~5 back to 4095 crosses the
// Up window on the way. An honest Up press has ~2248 behind it, never ~5.
//
// This is what keeps the instrument silent in daily use: it says nothing until
// it has actually caught something, and only then takes the footer over. A
// diagnostic that shouts every day stops being read.
static bool diagSuspectSeen = false;

static void buttonDiagSample() {
  if (!BUTTON_DIAG) return;
  const int v = adc1_get_raw(ADC1_CHANNEL_2);
  adcCh2Now = v;
  diagRing[diagRingPos] = v;
  diagRingPos = (diagRingPos + 1) % DIAG_RING;
  if (diagRingPos == 0) diagRingFilled = true;
}

// Called wherever an Up event is enqueued, so the evidence is captured at the
// instant the event happens rather than whenever the screen next redraws.
static void buttonDiagNoteUp() {
  if (!BUTTON_DIAG) return;
  upEventsSent++;
  upEventAdc = adcCh2Now;
  const int n = diagRingFilled ? DIAG_RING : diagRingPos;
  int lowest = 9999;
  for (int i = 0; i < n; i++) {
    if (diagRing[i] < lowest) lowest = diagRing[i];
  }
  upEventPreMin = (n > 0) ? lowest : -1;
  // 1120 is the top of the Down window in ADC_RANGES_2: anything at or below it
  // was Down, not Up, so an Up event with that in its history is suspect.
  if (n > 0 && lowest <= 1120) diagSuspectSeen = true;
}

bool buttonDiagSuspect() { return BUTTON_DIAG && diagSuspectSeen; }

// Shown only once buttonDiagSuspect() is true.
//   ev<count> @<value when it fired> pre<lowest just before>
void buttonDiagUpText(char* out, size_t n) {
  snprintf(out, n, "!up ev%u @%d pre%d", upEventsSent, upEventAdc, upEventPreMin);
}

// Raise the quick menu for whatever screen is up. Called from the button poll
// rather than through the key queue: the gesture is a hold, which has no key to
// stand for it, and inventing one would mean a keyboard could send it by
// accident.
static void openQuickMenuHere() {
  if (currentState == UIState::FILE_BROWSER) {
    if (noteVisibleCount() > 0) {
      quickOpen(quickMenu, QuickKind::Note, selectedFileIndex);
      screenDirty = true;
    }
  } else if (currentState == UIState::TEXT_EDITOR) {
    quickOpen(quickMenu, QuickKind::Editor, -1);
    screenDirty = true;
  }
}

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

  buttonDiagSample();

  // First call after boot: adopt whatever the buttons are doing as the baseline
  // instead of comparing against a `false` that was never observed. Without
  // this, a button already down at the first poll — a settling ADC, or simply
  // booting with Up held — reads as a rising edge and fires a press nobody made.
  //
  // Seeding rather than returning early: the power state machine below has to
  // see this poll too, or the release that arms `powerReleasedSinceWake` can be
  // the one we skipped. With Last == current, no edge fires this time round and
  // everything else runs normally.
  static bool baselineTaken = false;
  if (!baselineTaken) {
    baselineTaken = true;
    btnUpLast = btnUp;
    btnDownLast = btnDown;
    btnLeftLast = btnLeft;
    btnRightLast = btnRight;
    btnConfirmLast = btnConfirm;
    btnBackLast = btnBack;
  }

  // Power button state machine for proper long/short press handling
  static bool powerHeld = false;
  static unsigned long powerPressStart = 0;
  static bool sleepTriggered = false;
  // The wake press must not become a sleep gesture: see POWER_SLEEP_HOLD_MS.
  static bool powerReleasedSinceWake = false;

  bool btnPower = gpio.isPressed(HalGPIO::BTN_POWER);
  if (!btnPower) powerReleasedSinceWake = true;

  if (btnPower && !powerHeld) {
    // Button just pressed
    powerHeld = true;
    sleepTriggered = false;
    powerPressStart = millis();
  }

  if (btnPower && powerHeld && !sleepTriggered
      && powerReleasedSinceWake && millis() >= allowSleepAt) {
    if (millis() - powerPressStart > POWER_SLEEP_HOLD_MS) {
      sleepTriggered = true;
      enterDeepSleep(SleepReason::POWER_LONGPRESS);
      return; // Exit early to prevent further processing
    }
  }

  if (!btnPower && powerHeld) {
    // Button released
    unsigned long duration = millis() - powerPressStart;
    powerHeld = false;

    // The short-press window ends where the sleep hold begins — with the old
    // 1000ms ceiling against a 400ms hold, a half-second press would have been
    // both "go to the main menu" and "sleep".
    if (!sleepTriggered && duration > POWER_SHORT_PRESS_MIN_MS
        && duration < POWER_SLEEP_HOLD_MS) {
      // Short press - go to main menu (except when already there)
      if (currentState != UIState::MAIN_MENU) {
        if (currentState == UIState::WIFI_SYNC) {
          // Leaving WIFI_SYNC without this left syncActive true, the radio
          // up, and deep sleep suppressed forever. With the AP holding
          // ESP_PM_APB_FREQ_MAX the device would not light-sleep at all.
          wifiSyncStop();
        } else {
          if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
            saveCurrentFile();
          }
          currentState = UIState::MAIN_MENU;
          screenDirty = true;
        }
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
      if (currentState == UIState::WIFI_SYNC) {
        wifiSyncStop();
      }
      if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
        saveCurrentFile();
      }
      delay(100);
      ESP.restart();
    }
  }

  if (!btnBack && backHeld) {
    backHeld = false;
  }

  // Rotate once, here, instead of in every screen below. The old code carried
  // `|| btnRight` in six blocks — a landscape patch that is right in one
  // landscape and wrong in the other, and that left Left and Right doing double
  // duty everywhere.
  //
  // Edges are taken on the PHYSICAL buttons and only then rotated. Rotating the
  // held state instead and remembering it in logical space would manufacture an
  // edge the moment the orientation changed, which is exactly the phantom-press
  // shape this input path has already been bitten by once.
  const bool physEdge[4] = {btnUp && !btnUpLast, btnDown && !btnDownLast,
                            btnLeft && !btnLeftLast, btnRight && !btnRightLast};
  const bool physHeld[4] = {btnUp, btnDown, btnLeft, btnRight};

  bool dirEdge[4] = {false, false, false, false};
  bool dirHeld[4] = {false, false, false, false};
  for (int i = 0; i < 4; i++) {
    const Dir to = dpadResolve(static_cast<Dir>(i), currentOrientation, dpadMode);
    const int j = static_cast<int>(to);
    if (physEdge[i]) dirEdge[j] = true;
    if (physHeld[i]) dirHeld[j] = true;
  }

  // In Fixed mode Left and Right double as Down and Up on list screens, which is
  // the behaviour that shipped and the muscle memory that exists. In Natural
  // mode each button already points somewhere definite, so the alias would take
  // a direction away.
  const bool aliasing = (dpadMode == DpadMode::Fixed);
  const bool navUpEdge =
      dirEdge[(int)Dir::Up] || (aliasing && dirEdge[(int)Dir::Right]);
  const bool navDownEdge =
      dirEdge[(int)Dir::Down] || (aliasing && dirEdge[(int)Dir::Left]);

  // The select button carries two gestures on the two screens that have a quick
  // menu: a tap is Enter, a hold raises the menu. So a tap has to fire on
  // RELEASE there — firing it on the press would open the note first and then
  // the menu on top of it.
  //
  // Everywhere else it still fires on the press. The delay is small but real,
  // and those screens have nothing to trade it for.
  //
  // Same shape as the power button, and for the same reason: a gesture that
  // arms on a hold needs the release to decide what the press meant. The menu
  // being open disarms the hold, so select inside the menu is an ordinary
  // immediate Enter.
  static bool confirmHeld = false;
  static unsigned long confirmPressStart = 0;
  static bool confirmLongFired = false;

  const bool quickGesture = (currentState == UIState::FILE_BROWSER ||
                             currentState == UIState::TEXT_EDITOR) &&
                            !quickMenu.open() && !confirmDialog.open();

  bool confirmShort = false;
  if (btnConfirm && !btnConfirmLast) {
    confirmHeld = true;
    confirmPressStart = millis();
    confirmLongFired = false;
    if (!quickGesture) confirmShort = true;
  }
  if (btnConfirm && confirmHeld && !confirmLongFired && quickGesture &&
      millis() - confirmPressStart >= QUICK_MENU_HOLD_MS) {
    confirmLongFired = true;
    openQuickMenuHere();
  }
  if (!btnConfirm && confirmHeld) {
    confirmHeld = false;
    // A hold that already fired must not also send an Enter on the way up.
    if (quickGesture && !confirmLongFired) confirmShort = true;
  }

  // Map physical buttons to HID key codes based on current UI state
  switch (currentState) {
    case UIState::MAIN_MENU:
      if (navUpEdge) {
        buttonDiagNoteUp();
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (navDownEdge) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmShort) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      break;

    case UIState::FILE_BROWSER:
      if (navUpEdge && getFileCount() > 0) {
        buttonDiagNoteUp();
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (navDownEdge && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmShort && getFileCount() > 0) {
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
      if      (dirHeld[(int)Dir::Up])    heldKey = HID_KEY_UP;
      else if (dirHeld[(int)Dir::Down])  heldKey = HID_KEY_DOWN;
      else if (dirHeld[(int)Dir::Left])  heldKey = HID_KEY_LEFT;
      else if (dirHeld[(int)Dir::Right]) heldKey = HID_KEY_RIGHT;

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

      if (confirmShort) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        // Back leaves the editor by changing state here rather than through the
        // key queue, so with a box up it would walk out from under it — saving
        // the note the box was asking about deleting. With one open it becomes
        // the box's own way out instead.
        if (quickMenu.open() || confirmDialog.open()) {
          enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
          enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
        } else {
          if (editorHasUnsavedChanges()) saveCurrentFile();
          currentState = UIState::FILE_BROWSER;
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::RENAME_FILE:
    case UIState::NEW_FILE:
      if (confirmShort) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::BLUETOOTH_SETTINGS:
      // Left and Right mean Disconnect and Scan here, so this screen has no
      // alias either. Up and Down were left on the RAW buttons when the
      // rotation went in — one axis rotated and the other not, which in Natural
      // landscape had the two pairs obeying different rules on the same screen.
      if (dirEdge[(int)Dir::Up]) {
        buttonDiagNoteUp();
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (dirEdge[(int)Dir::Down]) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (dirEdge[(int)Dir::Right]) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);  // Scan
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (dirEdge[(int)Dir::Left]) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);   // Disconnect
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (confirmShort) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::PAIRED_KEYBOARDS:
      if (navUpEdge) {
        buttonDiagNoteUp();
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (navDownEdge) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmShort) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::WIFI_SYNC:
      if (navUpEdge) {
        buttonDiagNoteUp();
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (navDownEdge) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (confirmShort) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::SETTINGS:
      // The one list screen WITHOUT the Left/Right-as-Up/Down alias: since
      // Settings grew tabs it has two axes, and the alias would spend the
      // horizontal one. Up/Down walk the rows, Left/Right the tabs — and both
      // pairs come from dirEdge, so they follow the screen in Natural mode.
      if (dirEdge[(int)Dir::Up]) {
        buttonDiagNoteUp();
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (dirEdge[(int)Dir::Down]) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (dirEdge[(int)Dir::Left]) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (dirEdge[(int)Dir::Right]) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (confirmShort) {
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
//
// A wallpaper is attempted first; the built-in card is the fallback whenever
// there is no card, no /sleep folder, or nothing in it that parses as a BMP.
// On a device with no USB recovery, "asleep showing nothing" is a state worth
// never creating.
// Centered status card. HALF_REFRESH so it actually replaces the sleep
// wallpaper (a fast differential update ghosts the previous frame).
void drawStatusPopup(const char* msg) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const int tw = renderer.getTextAdvanceX(FONT_CHROME_XL, msg);
  const int th = renderer.getLineHeight(FONT_CHROME_XL);
  const int w = tw + 48;
  const int h = th + 28;
  const int x = (sw - w) / 2;
  const int y = sh / 2 - h / 2;
  renderer.fillRect(x - 3, y - 3, w + 6, h + 6, true);
  renderer.fillRect(x, y, w, h, false);
  renderer.drawText(FONT_CHROME_XL, x + 24, y + 8, msg, true, EpdFontFamily::BOLD);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void renderSleepScreen() {
  // The reading UI may be landscape, but the sleep image stays portrait:
  // logical 480×800, which the renderer rotates onto the panel. Restored
  // before we return so "Waking up..." follows the saved orientation.
  const auto savedOrientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Portrait);

  // Shown before the SD read and the dither, which are the slow part.
  // Same role as CrossInk's "Going to sleep..." popup.
  drawStatusPopup("Going to sleep...");

  if (sleepScreenDrawImage(renderer, sleepScreenMode)) {
    renderer.setOrientation(savedOrientation);
    delay(500);
    return;
  }

  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  // The sleep card is chrome, not note text, so it follows the chrome face.
  // Same ink-matched mapping as ui_renderer.cpp: L for what was notosans 14,
  // M for notosans 12, S for ubuntu 10. The x here is recomputed from the
  // measured width, so the wider monospace stays centred on its own.

  // Title: "Ardosia"
  const char* title = "Ardosia";
  int titleWidth = renderer.getTextAdvanceX(FONT_CHROME_XL, title);
  int titleX = (sw - titleWidth) / 2;
  int titleY = sh * 0.35; // 35% down the screen (moved up)
  renderer.drawText(FONT_CHROME_XL, titleX, titleY, title, true, EpdFontFamily::BOLD);

  // Subtitle: "Asleep"
  const char* subtitle = "Asleep";
  int subTitleWidth = renderer.getTextAdvanceX(FONT_CHROME_L, subtitle);
  int subTitleX = (sw - subTitleWidth) / 2;
  int subTitleY = sh * 0.48; // 48% down the screen (moved up)
  renderer.drawText(FONT_CHROME_L, subTitleX, subTitleY, subtitle, true);

  // Footer: "Hold Power to wake"
  const char* footer = "Hold Power to wake";
  int footerWidth = renderer.getTextAdvanceX(FONT_CHROME_S, footer);
  int footerX = (sw - footerWidth) / 2;
  int footerY = sh * 0.75; // 75% down the screen (moved up from bottom)
  renderer.drawText(FONT_CHROME_S, footerX, footerY, footer);

  // Perform a full display refresh to ensure the sleep screen is visible
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  renderer.setOrientation(savedOrientation);

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
  // A dialog belongs to the screen that raised it. The power button's short
  // press changes state without going through the key path, so without this a
  // "Delete this note?" would still be pending behind the main menu and would
  // reappear — armed — the next time the notes list was opened.
  if (currentState != lastState) {
    confirmClose(confirmDialog);
    quickClose(quickMenu);
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

  // Refresh the sync/hotspot screen only when a counter actually moved.
  // A multi-minute AP session used to force a FAST_REFRESH every 2 s.
  if (currentState == UIState::WIFI_SYNC) {
    static unsigned long lastSyncRefresh = 0;
    if (millis() - lastSyncRefresh > 2000) {
      lastSyncRefresh = millis();
      if (wifiSyncUiChanged()) screenDirty = true;
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
  static KeyboardLayout lastSavedKeyboardLayout = keyboardLayout;
  static SleepScreenMode lastSavedSleepScreen = sleepScreenMode;
  static SleepBrightness lastSavedSleepBrightness = sleepBrightness;
  static NoteSort lastSavedNoteSort = noteSort;
  static EditorFont lastSavedEditorFont = editorFont;
  static DpadMode lastSavedDpadMode = dpadMode;
  if (currentOrientation != lastSavedOrientation || darkMode != lastSavedDarkMode
      || writingMode != lastSavedWritingMode || fontSize != lastSavedFontSize
      || showWordCount != lastSavedShowWordCount
      || keyboardLayout != lastSavedKeyboardLayout
      || sleepScreenMode != lastSavedSleepScreen
      || sleepBrightness != lastSavedSleepBrightness
      || noteSort != lastSavedNoteSort
      || editorFont != lastSavedEditorFont
      || dpadMode != lastSavedDpadMode) {
    uiPrefs.putUChar("orient", static_cast<uint8_t>(currentOrientation));
    uiPrefs.putBool("darkMode", darkMode);
    uiPrefs.putUChar("writeMode", static_cast<uint8_t>(writingMode));
    uiPrefs.putUChar("fontSize", static_cast<uint8_t>(fontSize));
    uiPrefs.putBool("showWC", showWordCount);
    uiPrefs.putUChar("kbLayout", static_cast<uint8_t>(keyboardLayout));
    uiPrefs.putUChar("sleepScr", static_cast<uint8_t>(sleepScreenMode));
    uiPrefs.putUChar("sleepLight", static_cast<uint8_t>(sleepBrightness));
    uiPrefs.putUChar("noteSort", static_cast<uint8_t>(noteSort));
    uiPrefs.putUChar("editFont", static_cast<uint8_t>(editorFont));
    uiPrefs.putUChar("dpad", static_cast<uint8_t>(dpadMode));
    lastSavedOrientation = currentOrientation;
    lastSavedDarkMode = darkMode;
    lastSavedWritingMode = writingMode;
    lastSavedFontSize = fontSize;
    lastSavedShowWordCount = showWordCount;
    lastSavedKeyboardLayout = keyboardLayout;
    lastSavedSleepScreen = sleepScreenMode;
    lastSavedSleepBrightness = sleepBrightness;
    lastSavedNoteSort = noteSort;
    lastSavedEditorFont = editorFont;
    lastSavedDpadMode = dpadMode;
    // Keep SD backup in sync so settings survive a firmware flash
    static char uiBuf[256];
    snprintf(uiBuf, sizeof(uiBuf),
             "{\"orient\":%d,\"dark\":%d,\"writeMode\":%d,\"fontSize\":%d,"
             "\"showWC\":%d,\"kbLayout\":%d,\"sleepScr\":%d,\"sleepLight\":%d,"
             "\"noteSort\":%d,\"editFont\":%d,\"dpad\":%d}",
             (int)currentOrientation, darkMode ? 1 : 0,
             (int)writingMode, (int)fontSize, showWordCount ? 1 : 0,
             (int)keyboardLayout, (int)sleepScreenMode, (int)sleepBrightness,
             (int)noteSort, (int)editorFont, (int)dpadMode);
    sdEnsureBackupDir();
    sdWriteFile("/ardosia/ui_prefs.json", uiBuf);
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
