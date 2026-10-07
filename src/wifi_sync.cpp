#include "wifi_sync.h"
#include "config.h"
#include "file_manager.h"
#include <esp_heap_caps.h>
#include <esp_netif.h>
#include <esp_pm.h>

#include "ble_keyboard.h"
#include "text_editor.h"
#include "note_json.h"
#include "note_html.h"
#include "web_page.h"
#include "zip_writer.h"
#include "hotspot_creds.h"
#include "sd_backup.h"
#include "utf8_util.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <SDCardManager.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>

// --- Internal state ---
static WebServer* server = nullptr;
static bool syncActive = false;
static SyncState syncState = SyncState::SCANNING;
static char statusText[64] = "";

extern bool screenDirty;

// --- Network list ---
static constexpr int MAX_NETWORKS = 20;
struct NetworkInfo {
  char ssid[33];
  int  rssi;
  bool encrypted;
  bool saved;  // Has stored password in NVS
};
static NetworkInfo networks[MAX_NETWORKS];
static int networkCount = 0;
static int selectedNet = 0;

// --- Password entry ---
static constexpr int MAX_PASSWORD_LEN = 63;
static char passwordBuf[MAX_PASSWORD_LEN + 1] = "";
static int  passwordLen = 0;

// --- NVS credential storage ---
static Preferences wifiPrefs;
static constexpr int MAX_SAVED_NETWORKS = 4;

static void loadSavedCredentials();
static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize);
static void saveCredential(const char* ssid, const char* pass);
static void forgetCredential(const char* ssid);

// --- Connecting state ---
static unsigned long connectStartMs = 0;
static char connectingSSID[33] = "";
static bool usedSavedPassword = false;
static bool autoConnectAttempted = false;  // True if we tried auto-connect with saved creds

// --- Sync activity tracking ---
static int filesSent = 0;       // Files downloaded by PC (GET)
static int filesReceived = 0;  // Files uploaded by PC (POST)
static int totalFilesToSync = 0; // Total .txt files on device

static constexpr int MAX_LOG_LINES = 6;
static char syncLog[MAX_LOG_LINES][48];
static int syncLogCount = 0;

static bool pcConnected = false;

static unsigned long lastHttpActivityMs = 0;
static constexpr unsigned long SYNC_TIMEOUT_MS = 60000;  // 60s no HTTP → auto-disconnect
static bool syncCompletePending = false;  // Set by handler, acted on in wifiSyncLoop
static bool notesRefreshPending = false;  // POST /notes wrote; refresh list in the loop

// SoftAP — adapted from CrossPointWebServerActivity::startAccessPoint
// (CrossInk, MIT). Diverges: WPA2 password (they use an open network),
// no DNSServer captive portal, no QR code. The X3 joins by this SSID
// and posts to the fixed IP.
//
// NEITHER the SSID NOR the passphrase is a constant any more, and both changes
// are about two devices rather than one:
//
//   SSID        "Ardosia-XXXX" from the soft-AP MAC. Two units in the same room
//               broadcasting one SSID with different passphrases make a phone
//               fail to join in a way it cannot explain — it picks whichever
//               beacon is louder. A writerdeck meetup is a realistic case.
//   passphrase  minted per device, kept in NVS. What was here before was
//               "ardosia12" on every unit, published in a public repository,
//               on an AP that accepts POST /notes.
static constexpr char AP_SSID_PREFIX[] = "Ardosia-";
static constexpr int AP_CHANNEL = 1;

// "Ardosia-" + 4 hex + NUL = 13. Derived at runtime, never stored: the MAC is
// in eFuse and cannot drift, so a copy in NVS would only be a second thing to
// keep in step.
static char apSsid[16] = "";

// NVS, and nowhere else. NOT the SD card: the card is removable and the whole
// point of F3's fix is that a passphrase on removable media is a passphrase
// anyone who handles the card has. Never logged either — no DBG_PRINTF below
// prints this buffer, and the AP-up line prints the SSID only.
static constexpr char AP_NVS_NS[]  = "ardosia_ap";
static constexpr char AP_NVS_KEY[] = "psk";
static char apPassword[HOTSPOT_PW_BUFLEN] = "";
static constexpr int AP_MAX_CONN = 4;
static unsigned long apStartMs = 0;
static constexpr unsigned long AP_NO_CLIENT_MS = 5UL * 60UL * 1000UL;
static int lastSeenStations = -1;

// =========================================================================
// Hotspot identity: the SSID and the passphrase
// =========================================================================

// esp_random() is a TRUE random source only while the RF subsystem is powered;
// with the radio down it degrades to a PRNG seeded at boot. Every call site
// below therefore mints a passphrase only with WIFI_AP already up — see
// apEnsurePassword()'s callers.
static uint32_t apRng() { return esp_random(); }

// The MAC comes from eFuse, so this works before WiFi is initialised — which
// matters because the hotspot screen asks for the SSID to draw it.
static const char* apEnsureSsid() {
  if (apSsid[0] == '\0') {
    uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
      // No MAC to read is not a reason to have no hotspot. The bare prefix is
      // the old behaviour, which worked for everyone with one device.
      snprintf(apSsid, sizeof(apSsid), "Ardosia");
    } else {
      snprintf(apSsid, sizeof(apSsid), "%s%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    }
  }
  return apSsid;
}

// Load the stored passphrase, or mint and store one on first use.
//
// CALL THIS ONLY WITH THE RADIO UP. The stored value is reused for the life of
// the device, so the one draw that matters is the first one, and drawing it from
// a PRNG would make every unit flashed from the same image agree.
static void apEnsurePassword() {
  if (apPassword[0] != '\0') return;

  Preferences apPrefs;
  if (!apPrefs.begin(AP_NVS_NS, false)) {
    // NVS unavailable: mint one for this session rather than fall back to a
    // shared constant. It is lost on reboot, which is visible and safe; a
    // hardcoded default would be neither.
    hotspotGeneratePassword(apPassword, sizeof(apPassword), apRng);
    DBG_PRINTLN("[SYNC] AP NVS unavailable - passphrase is session-only");
    return;
  }

  const String stored = apPrefs.getString(AP_NVS_KEY, "");
  if (stored.length() == HOTSPOT_PW_LEN) {
    strncpy(apPassword, stored.c_str(), sizeof(apPassword) - 1);
    apPassword[sizeof(apPassword) - 1] = '\0';
  } else {
    // Absent, or the wrong length for any reason: mint a fresh one. A
    // half-written key must never become a short WPA2 passphrase.
    hotspotGeneratePassword(apPassword, sizeof(apPassword), apRng);
    if (apPassword[0] != '\0') apPrefs.putString(AP_NVS_KEY, apPassword);
    DBG_PRINTLN("[SYNC] Minted a new hotspot passphrase");
  }
  apPrefs.end();
}

// =========================================================================
// Keeping the soft AP awake, and off the air with BLE
// =========================================================================
//
// MEASURED, not guessed. Symptom: a laptop could see the hotspot, associate,
// and never finish connecting. Two distinct failures, one root.
//
//   With the Bluetooth keyboard ON:
//     Associated with e0:72:a1:83:c0:1d
//     CTRL-EVENT-DISCONNECTED reason=15        <- 4WAY_HANDSHAKE_TIMEOUT
//   three times, exactly 4 s apart each time — the EAPOL retry budget expiring.
//   NOT reason=WRONG_KEY: the passphrase was never rejected, it was never
//   finished being tested. The ESP32-C3 has ONE 2.4 GHz radio, and a BLE link
//   at a 30-50 ms interval does not leave EAPOL a window.
//
//   With the keyboard OFF the handshake completes — and then:
//     dhcp4: beginning transaction (timeout in 45 seconds)
//     dhcp4: state changed no lease                     (three times)
//     device: ip-config -> failed (reason 'ip-config-unavailable')
//   The link is up at layer 2 and the AP never answers DHCP.
//
// That is the shape of an AP that is asleep between beacons:
//   - beacons are scheduled, so the AP is VISIBLE;
//   - association retries until one lands in an awake window, so it ASSOCIATES;
//   - EAPOL needs several round trips inside ~4 s, so it is marginal;
//   - an inbound DHCP DISCOVER arriving during a sleep window is simply lost,
//     and 45 s of retries all miss.
//
// We asked for that: esp_pm_configure(light_sleep_enable = true) in main.cpp is
// global, and CONFIG_ESP_PHY_MAC_BB_PD powers the MAC and baseband down in
// light sleep. A soft AP cannot be a soft AP under those conditions.
//
// THE LOCK TYPE MATTERS. main.cpp used to reason that "the AP holding
// ESP_PM_APB_FREQ_MAX" meant no light sleep. Those are different locks
// (esp_pm.h:44-54): APB_FREQ_MAX pins the APB clock, ESP_PM_NO_LIGHT_SLEEP is
// the one that prevents light sleep. Even if the Wi-Fi driver takes the first,
// it does not get us the second.
//
// Battery cost, stated as this repo requires: the lock is held ONLY while the
// AP is up, which is a modal screen with a 5-minute no-client timeout and a
// 60-second idle timeout. Outside the hotspot nothing changes.
static esp_pm_lock_handle_t apNoSleepLock = nullptr;
static bool apNoSleepHeld = false;

// --- Heap readout on the hotspot screen ------------------------------------
//
// This is what is left of the instrument that found bug #7, and it stays until
// the export feature is finished. Free heap on the left, largest contiguous
// block on the right — the second number matters more, because a fragmented
// heap with plenty free in small pieces fails an allocation exactly like an
// empty one does.
//
// It earns its place because the failure mode here is silent and expensive:
// below roughly 15k the DHCP server cannot allocate a packet to answer a lease
// and accept() cannot allocate a client, while LwIP still completes the TCP
// handshake from its own PCB pool. That reads as "the server is broken" and
// cost four wrong hypotheses before anyone thought to look at the heap.
//
// Goes out with the other temporaries before release — see Backlog.
void apHttpStatusText(char* out, size_t n) {
  if (!out || n == 0) return;
  const uint32_t freeKb = esp_get_free_heap_size() / 1024;
  const uint32_t biggestKb =
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024;
  snprintf(out, n, "heap:%luk/%luk", (unsigned long)freeKb,
           (unsigned long)biggestKb);
}

// Is the AP's DHCP server actually running, and on what address?
//
// An instrument, not decoration. "Associates and never gets an address" had
// three plausible explanations and I guessed wrong twice; this reports the fact
// instead. Shown on the hotspot screen so it is readable on a device with no
// serial console.
//
// Returns false when there is no AP netif at all, which is itself an answer.
bool apDhcpStatusText(char* out, size_t n) {
  if (!out || n == 0) return false;
  out[0] = '\0';
  esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap) {
    snprintf(out, n, "DHCP: no netif");
    return false;
  }
  esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
  const bool got = esp_netif_dhcps_get_status(ap, &status) == ESP_OK;
  const char* word = !got                              ? "?"
                     : status == ESP_NETIF_DHCP_STARTED ? "on"
                     : status == ESP_NETIF_DHCP_STOPPED ? "STOPPED"
                                                        : "init";

  // The ADDRESS, read live from the netif, not from the cached statusText.
  //
  // Reported together with the state because reporting the state alone measured
  // the wrong half: "DHCP: on" was true while the address had never been
  // applied, so the instrument agreed with a broken AP. A running server on the
  // wrong address answers nobody.
  esp_netif_ip_info_t ip = {};
  if (esp_netif_get_ip_info(ap, &ip) == ESP_OK) {
    snprintf(out, n, "DHCP: %s " IPSTR, word, IP2STR(&ip.ip));
  } else {
    snprintf(out, n, "DHCP: %s (no ip)", word);
  }
  return got && status == ESP_NETIF_DHCP_STARTED;
}

static void apHoldAwake() {
  if (apNoSleepHeld) return;
  if (apNoSleepLock == nullptr &&
      esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ardosia_ap", &apNoSleepLock) != ESP_OK) {
    DBG_PRINTLN("[SYNC] Could not create the no-light-sleep lock");
    return;
  }
  if (esp_pm_lock_acquire(apNoSleepLock) == ESP_OK) apNoSleepHeld = true;
}

// Must be reached by EVERY path that takes the AP down, or the device never
// light-sleeps again and the battery pays for it silently. radioOff() is that
// single chokepoint: enterDoneState() and wifiSyncStop() both go through it.
static void apReleaseAwake() {
  if (!apNoSleepHeld) return;
  esp_pm_lock_release(apNoSleepLock);
  apNoSleepHeld = false;
}

// Get BLE off the air for the duration. Disconnect, not NimBLEDevice::deinit():
// bug #2 of this fork was a panic in the NimBLE host task from touching its API
// in the wrong window, and a deinit/init cycle around the radio is the easiest
// way to find that again. These are the same calls the Bluetooth settings
// screen already makes from the loop task.
//
// autoReconnectEnabled is NOT set here. main.cpp owns it by UI state and
// rewrites it every loop iteration, so setting it from this side would be
// overwritten within milliseconds; the gate for WIFI_SYNC lives there.
// How long to wait for BLE to actually let go. A connect task runs up to
// CONNECT_TIMEOUT_MS (10 s), but waiting that long freezes the screen; the
// retry in raiseAp() covers the rest of the window.
static constexpr unsigned long BLE_YIELD_MS = 3000;

static bool bleOffTheAirForAp() {
  // Set here, synchronously, and NOT left to main.cpp.
  //
  // main.cpp also holds this false by UI state — but that block runs BEFORE
  // processAllInput() in the loop, so on the very iteration that enters the
  // hotspot it is still true, and that is the iteration raiseAp() runs in.
  // Which is exactly the observed failure: "AP Failed" immediately after boot,
  // then the hotspot working fine a few minutes later. Right after boot
  // reconnectDelay is 5 s and a connect task fires every few seconds; by the
  // time the backoff has climbed to its 2-minute cap the radio is mostly idle
  // and softAP() finds it free. main.cpp's gate keeps it false afterwards.
  autoReconnectEnabled = false;

  if (isDeviceScanning()) stopDeviceScan();
  cancelPendingConnection();
  if (isKeyboardConnected()) {
    disconnectCurrentDevice();
    DBG_PRINTLN("[SYNC] Keyboard disconnected so the AP can have the radio");
  }

  // And then WAIT. cancelPendingConnection() only prevents NEW attempts — a
  // task already running keeps the radio, by its own admission. Calling
  // softAP() on top of one is what fails.
  const unsigned long until = millis() + BLE_YIELD_MS;
  while (bleRadioBusy() && millis() < until) delay(20);
  // "Trying anyway" is what this used to do, and it is what left the keyboard
  // dead until a reboot. BLE_YIELD_MS is 3 s; CONNECT_TIMEOUT_MS is 10 s, and
  // service discovery runs on top of that — the wait was always shorter than
  // the thing it waits for. Taking the radio from a live connect task leaves it
  // blocked inside connect()/secureConnection(), so connectTaskHandle never
  // returns to nullptr. Every gate that starts a connection tests that handle,
  // so afterwards both auto-reconnect AND the manual Scan in Settings do
  // nothing, silently, with the stack perfectly healthy. That is the symptom.
  //
  // Waiting the full 13 s would block the UI on a modal screen, so: refuse.
  // A hotspot that does not start costs one button press; a keyboard that
  // needs a reboot cannot be recovered from the device at all.
  if (bleRadioBusy()) {
    DBG_PRINTLN("[SYNC] BLE connect task holds the radio; refusing to start AP");
    return false;
  }
  return true;

}

static void radioOff() {
  apReleaseAwake();
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

static String currentSyncIp() {
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_AP || mode == WIFI_AP_STA) return WiFi.softAPIP().toString();
  return WiFi.localIP().toString();
}

// --- DONE state ---
static unsigned long doneStartMs = 0;
static constexpr unsigned long DONE_DISPLAY_MS = 3000;  // 3s before returning to menu

// --- Forward declarations ---
static void startHttpServer();
static void stopHttpServer();
static void beginScan();
static void beginConnect(const char* ssid, const char* pass);
static void enterSyncingState();
static void enterDoneState();
static void addSyncLogEntry(const char* fmt, const char* filename);

// =========================================================================
// Sync log helpers
// =========================================================================

static void resetSyncTracking() {
  filesSent = 0;
  filesReceived = 0;
  totalFilesToSync = getFileCount();
  syncLogCount = 0;
  syncCompletePending = false;
  pcConnected = false;
  for (int i = 0; i < MAX_LOG_LINES; i++) syncLog[i][0] = '\0';
}

int getSyncTotalFiles() { return totalFilesToSync; }

static void addSyncLogEntry(const char* fmt, const char* filename) {
  // Shift entries up if full
  if (syncLogCount >= MAX_LOG_LINES) {
    for (int i = 0; i < MAX_LOG_LINES - 1; i++) {
      strncpy(syncLog[i], syncLog[i + 1], sizeof(syncLog[i]) - 1);
      syncLog[i][sizeof(syncLog[i]) - 1] = '\0';
    }
    syncLogCount = MAX_LOG_LINES - 1;
  }
  snprintf(syncLog[syncLogCount], sizeof(syncLog[syncLogCount]), fmt, filename);
  syncLogCount++;
  screenDirty = true;
}

// =========================================================================
// The SD backup of Wi-Fi credentials — removed (F3)
// =========================================================================
//
// writeWifiBackup() and restoreWifiBackup() used to mirror the STA SSIDs and
// passwords into /ardosia/wifi.json so they would survive an NVS wipe. They are
// gone, and the file is deleted on boot.
//
// WHY NOT JUST ENCRYPT IT: the backup existed for exactly one case, a full flash
// erase, and a key kept in NVS is erased by that same event. An encrypted backup
// would protect nothing it needs to protect and restore nothing it exists to
// restore. What it did do is put the passwords of the user's OTHER networks —
// their home Wi-Fi — in plaintext on removable media, readable by anyone who
// picks up the card, and (with F1) by anyone in radio range.
//
// THE COST, stated plainly: after a firmware reflash wipes NVS, the user types
// the STA Wi-Fi password again. STA Sync is behind MENU_SHOW_SYNC today, so in
// practice almost nobody pays it.
static constexpr char WIFI_BACKUP_PATH[] = "/ardosia/wifi.json";

void wifiSyncMigrateDropSdBackup() {
  if (!SdMan.exists(WIFI_BACKUP_PATH)) return;
  const bool gone = SdMan.remove(WIFI_BACKUP_PATH);
  // HONEST LIMIT: FAT unlinks the directory entry and leaves the data sectors
  // alone, so the bytes are still recoverable from the raw card until something
  // overwrites them. Deleting is what this firmware can do; a card that has
  // held credentials cannot be un-held.
  DBG_PRINTF("[SYNC] Legacy wifi.json %s\n", gone ? "deleted" : "could NOT be deleted");
}

// =========================================================================
// NVS credential storage
// =========================================================================

static void loadSavedCredentials() {
  // Mark networks that have saved passwords
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < networkCount; i++) {
    networks[i].saved = false;
    for (int j = 0; j < count && j < MAX_SAVED_NETWORKS; j++) {
      char key[16];
      snprintf(key, sizeof(key), "wifi_ssid_%d", j);
      String savedSSID = wifiPrefs.getString(key, "");
      if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), networks[i].ssid) == 0) {
        networks[i].saved = true;
        break;
      }
    }
  }
}

static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      String savedPass = wifiPrefs.getString(pKey, "");
      strncpy(passBuf, savedPass.c_str(), passBufSize - 1);
      passBuf[passBufSize - 1] = '\0';
      return true;
    }
  }
  return false;
}

static void saveCredential(const char* ssid, const char* pass) {
  int count = wifiPrefs.getInt("wifi_count", 0);

  // Check if already saved — update in place
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      wifiPrefs.putString(pKey, pass);
      return;
    }
  }

  // Add new entry (wrap around if full)
  int slot = count < MAX_SAVED_NETWORKS ? count : (count % MAX_SAVED_NETWORKS);
  char sKey[16], pKey[16];
  snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", slot);
  snprintf(pKey, sizeof(pKey), "wifi_pass_%d", slot);
  wifiPrefs.putString(sKey, ssid);
  wifiPrefs.putString(pKey, pass);
  if (count < MAX_SAVED_NETWORKS) {
    wifiPrefs.putInt("wifi_count", count + 1);
  }
}

static void forgetCredential(const char* ssid) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      // Shift remaining entries down
      for (int j = i; j < count - 1 && j < MAX_SAVED_NETWORKS - 1; j++) {
        char srcS[16], srcP[16], dstS[16], dstP[16];
        snprintf(srcS, sizeof(srcS), "wifi_ssid_%d", j + 1);
        snprintf(srcP, sizeof(srcP), "wifi_pass_%d", j + 1);
        snprintf(dstS, sizeof(dstS), "wifi_ssid_%d", j);
        snprintf(dstP, sizeof(dstP), "wifi_pass_%d", j);
        wifiPrefs.putString(dstS, wifiPrefs.getString(srcS, ""));
        wifiPrefs.putString(dstP, wifiPrefs.getString(srcP, ""));
      }
      // Clear last slot
      int lastIdx = count - 1;
      char lastS[16], lastP[16];
      snprintf(lastS, sizeof(lastS), "wifi_ssid_%d", lastIdx);
      snprintf(lastP, sizeof(lastP), "wifi_pass_%d", lastIdx);
      wifiPrefs.remove(lastS);
      wifiPrefs.remove(lastP);
      wifiPrefs.putInt("wifi_count", count - 1);
      return;
    }
  }
}

// =========================================================================
// WiFi scanning
// =========================================================================

static void beginScan() {
  syncState = SyncState::SCANNING;
  strcpy(statusText, "Scanning...");
  networkCount = 0;
  selectedNet = 0;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.scanNetworks(true);  // async scan
  screenDirty = true;
  DBG_PRINTLN("[SYNC] WiFi scan started");
}

static void processScanResults() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;  // Still scanning

  if (n <= 0) {
    // Scan failed or no networks found
    networkCount = 0;
    syncState = SyncState::NETWORK_LIST;
    strcpy(statusText, n == 0 ? "No networks found" : "Scan failed");
    WiFi.scanDelete();
    screenDirty = true;
    return;
  }

  // Deduplicate by SSID, keeping strongest signal
  networkCount = 0;
  for (int i = 0; i < n && networkCount < MAX_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // Skip hidden networks

    // Check for duplicate
    bool duplicate = false;
    for (int j = 0; j < networkCount; j++) {
      if (strcmp(networks[j].ssid, ssid.c_str()) == 0) {
        duplicate = true;
        if (WiFi.RSSI(i) > networks[j].rssi) {
          networks[j].rssi = WiFi.RSSI(i);
        }
        break;
      }
    }
    if (duplicate) continue;

    strncpy(networks[networkCount].ssid, ssid.c_str(), 32);
    networks[networkCount].ssid[32] = '\0';
    networks[networkCount].rssi = WiFi.RSSI(i);
    networks[networkCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    networks[networkCount].saved = false;
    networkCount++;
  }

  WiFi.scanDelete();

  // Mark saved networks
  loadSavedCredentials();

  // Sort: saved networks first, then by signal strength
  for (int i = 0; i < networkCount - 1; i++) {
    for (int j = i + 1; j < networkCount; j++) {
      bool swap = false;
      if (networks[j].saved && !networks[i].saved) {
        swap = true;
      } else if (networks[j].saved == networks[i].saved && networks[j].rssi > networks[i].rssi) {
        swap = true;
      }
      if (swap) {
        NetworkInfo tmp = networks[i];
        networks[i] = networks[j];
        networks[j] = tmp;
      }
    }
  }

  selectedNet = 0;
  syncState = SyncState::NETWORK_LIST;
  statusText[0] = '\0';
  screenDirty = true;
  DBG_PRINTF("[SYNC] Found %d networks\n", networkCount);

  // Intelligent auto-connect: if the strongest available network is saved, auto-connect
  if (networkCount > 0 && networks[0].saved) {
    char savedPass[MAX_PASSWORD_LEN + 1];
    if (getSavedPassword(networks[0].ssid, savedPass, sizeof(savedPass))) {
      usedSavedPassword = true;
      autoConnectAttempted = true;
      beginConnect(networks[0].ssid, savedPass);
      DBG_PRINTF("[SYNC] Auto-connecting to strongest saved network: %s\n", networks[0].ssid);
      return;
    }
  }
}

// =========================================================================
// Connection
// =========================================================================

static void beginConnect(const char* ssid, const char* pass) {
  strncpy(connectingSSID, ssid, 32);
  connectingSSID[32] = '\0';
  syncState = SyncState::CONNECTING;
  snprintf(statusText, sizeof(statusText), "Connecting to %s...", ssid);
  connectStartMs = millis();

  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(ssid, pass);
  screenDirty = true;
  DBG_PRINTF("[SYNC] Connecting to %s\n", ssid);
}

static void enterSyncingState() {
  resetSyncTracking();
  startHttpServer();
  snprintf(statusText, sizeof(statusText), "%s",
           currentSyncIp().c_str());
  syncState = SyncState::SYNCING;
  lastHttpActivityMs = millis();
  screenDirty = true;
  DBG_PRINTF("[SYNC] Syncing — server at %s\n", statusText);
}

static void enterDoneState() {
  stopHttpServer();
  radioOff();

  syncState = SyncState::DONE;
  doneStartMs = millis();

  if (filesSent == 0 && filesReceived == 0) {
    strcpy(statusText, "No changes");
  } else {
    snprintf(statusText, sizeof(statusText), "Sent: %d  Received: %d",
             filesSent, filesReceived);
  }
  screenDirty = true;
  DBG_PRINTF("[SYNC] Done — %s\n", statusText);
}

static void pollConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    // If we used a manually entered password, prompt to save first
    if (!usedSavedPassword) {
      syncState = SyncState::SAVE_PROMPT;
      snprintf(statusText, sizeof(statusText), "%s",
               currentSyncIp().c_str());
      screenDirty = true;
    } else {
      enterSyncingState();
    }
    return;
  }

  if (millis() - connectStartMs > 25000) {
    WiFi.disconnect(true);
    strcpy(statusText, "Connection failed");

    if (usedSavedPassword) {
      syncState = SyncState::FORGET_PROMPT;
    } else {
      syncState = SyncState::CONNECT_FAILED;
    }

    screenDirty = true;
    DBG_PRINTLN("[SYNC] Connection timed out");
  }
}

// =========================================================================
// HTTP Server
// =========================================================================

static void handleFileList() {
  lastHttpActivityMs = millis();
  if (!pcConnected) {
    pcConnected = true;
    screenDirty = true;
  }

  auto dir = SdMan.open("/notes");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    server->send(500, "application/json", "[]");
    return;
  }

  // Streamed, not accumulated. This used to build the whole array in a heap
  // String and hand it to server->send(), which copies it again: ~8 KB for 200
  // notes and ~39 KB for 1000, doubled — and String grows by realloc, so the
  // real cost is the fragmentation as much as the peak, on a heap that is also
  // holding the AP, this server, a 16 KB text buffer and NimBLE.
  //
  // It survived because the only caller was sync.py on a developer's card. The
  // browser export page (../SPEC-notes-export.md) makes it the hot path, and
  // users accumulate notes, so it had to go first.
  //
  // CONTENT_LENGTH_UNKNOWN selects chunked transfer encoding: the length is not
  // known until the directory has been walked, and walking it twice to find out
  // would double the SD reads to save nothing.
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char name[256];
  char entry[NOTE_JSON_ENTRY_MAX];
  bool first = true;

  dir.rewindDirectory();
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    file.getName(name, sizeof(name));
    const unsigned long sz = (unsigned long)file.size();
    file.close();

    if (!noteJsonIncludes(name)) continue;

    // Zero means the entry would not fit, and the contract is to SKIP it, not
    // to write a partial one: a truncated entry corrupts the array from there
    // on and takes every note after it, which is what F5 actually did. Losing
    // one pathological name is survivable; a broken response is not.
    const size_t len = noteJsonEntry(name, sz, first, entry, sizeof(entry));
    if (len == 0) {
      DBG_PRINTF("[SYNC] Skipped unrepresentable name in list: %s\n", name);
      continue;
    }
    server->sendContent(entry, len);
    first = false;
  }
  dir.close();

  server->sendContent("]");
  server->sendContent("");  // zero-length chunk: ends a chunked response
}

// The ONLY place a GET turns a URI into a path on the card. Nothing else may
// build an SD path from a request: noteUriNameIsSafe() is the whole check and
// it runs before the first SD call, not after it.
static void handleFileDownload() {
  lastHttpActivityMs = millis();

  String uri = server->uri();
  if (!uri.startsWith("/notes/") || uri.length() <= 7) {
    server->send(400, "text/plain", "Bad request");
    return;
  }

  String filename = uri.substring(7);
  if (!noteUriNameIsSafe(filename.c_str())) {
    // 400, not 404: the name is malformed, and a 404 would tell the caller
    // something about which paths exist.
    server->send(400, "text/plain", "Bad request");
    DBG_PRINTLN("[SYNC] Rejected unsafe GET path");
    return;
  }

  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename.c_str());

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    server->send(404, "text/plain", "Not found");
    return;
  }

  size_t fileSize = file.size();
  server->setContentLength(fileSize);
  server->send(200, "text/plain", "");

  uint8_t buf[512];
  while (file.available()) {
    int bytesRead = file.read(buf, sizeof(buf));
    if (bytesRead <= 0) break;
    server->client().write(buf, bytesRead);
  }
  file.close();

  // Track: PC downloaded a file from device = "sent"
  filesSent++;
  screenDirty = true;
  DBG_PRINTF("[SYNC] Sent file: %s\n", filename.c_str());
}

// =========================================================================
// GET /api/export.zip — every note as one archive
// =========================================================================
//
// Streamed, never assembled. The format writer is src/zip_writer.h (pure,
// host-tested against Python's zipfile); everything here is the SD and HTTP
// half.
//
// THREE WALKS OF THE FOLDER, which is the design and not an accident:
//
//   1. names and sizes only, no file reads, to compute the exact
//      Content-Length. A ZIP's total size is pure arithmetic over names and
//      sizes — no CRC needed — so this is cheap, and it buys a real
//      Content-Length instead of chunked encoding: the browser gets a progress
//      bar and knows when the download is complete.
//   2. local headers and file data, CRC computed as we go.
//   3. central-directory headers, CRC computed again.
//
// Walks 2 and 3 each read every note, because a ZIP's local header carries the
// CRC BEFORE the data and the central directory repeats it at the end. The
// alternative is remembering per-entry state, which is ~64 KB for 1000 notes —
// the same unbounded-memory trap handleFileList() just had removed. Reading a
// 16 KB note twice costs nothing against sending it over the soft AP.
//
// The three walks are safe to assume identical because the whole export is ONE
// server->handleClient() call on the single Arduino loop task: no POST can add
// a note between them, and the autosave cannot fire because it is gated on
// UIState::TEXT_EDITOR while this screen is UIState::WIFI_SYNC. Same
// single-task argument that settled the autosave question.
//
// Entry order is FAT directory order. Sorting would mean holding every name in
// RAM, which is the thing being avoided; an archive has no natural order and
// the browser will show whatever the user's unzip tool shows.

// The ZIP writer emits 2- and 4-byte fields. Handing each one straight to the
// socket would be a syscall per field, so the sink coalesces into one block.
struct ZipHttpSink {
  uint8_t buf[512];
  size_t used = 0;
  bool ok = true;
};

static bool zipHttpFlush(ZipHttpSink* s) {
  if (!s->ok) return false;
  if (s->used == 0) return true;
  const size_t wrote = server->client().write(s->buf, s->used);
  if (wrote != s->used) { s->ok = false; return false; }
  s->used = 0;
  return true;
}

static bool zipHttpSink(const void* data, size_t len, void* ctx) {
  ZipHttpSink* s = static_cast<ZipHttpSink*>(ctx);
  if (!s->ok) return false;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  while (len > 0) {
    const size_t room = sizeof(s->buf) - s->used;
    const size_t take = len < room ? len : room;
    memcpy(s->buf + s->used, p, take);
    s->used += take;
    p += take;
    len -= take;
    if (s->used == sizeof(s->buf) && !zipHttpFlush(s)) return false;
  }
  return true;
}

// CRC-32 of a note, by the same routine the writer uses — zip_writer.cpp owns
// the only implementation on purpose, so host and device cannot disagree.
static bool zipCrcOfNote(const char* path, uint32_t* out) {
  auto f = SdMan.open(path, O_RDONLY);
  if (!f) return false;
  uint32_t crc = ZIP_CRC_INIT;
  uint8_t buf[512];
  while (f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    crc = zipCrc32Update(crc, buf, static_cast<size_t>(n));
  }
  f.close();
  *out = zipCrc32Final(crc);
  return true;
}

// FAT date and time for an entry. Always zero, deliberately: SdFat only stamps
// modifyDate/modifyTime when a FsDateTime callback is registered
// (FatFile.cpp:1254), and nothing registers one because the X4 has no RTC. So
// getModifyDateTime() would hand back a field nobody ever wrote, identical for
// every note. Zero reads as 1980-01-01 in every unzip tool, which is a
// recognisable "no date" rather than a wrong one.
//
// The order to fix this, if it ever matters: register the callback first (SNTP
// over the STA sync mode, or a date the user sets), and only then plumb the
// timestamp through here. Plumbing it first would just move the lie.
static constexpr uint16_t ZIP_NO_DATE = 0;
static constexpr uint16_t ZIP_NO_TIME = 0;

static void handleExportZip() {
  lastHttpActivityMs = millis();
  if (!pcConnected) {
    pcConnected = true;
    screenDirty = true;
  }

  // --- walk 1: the exact length, from names and sizes alone ---------------
  auto dir = SdMan.open("/notes");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    server->send(500, "text/plain", "No notes folder");
    return;
  }

  char name[256];
  ZipSizer sizer;
  dir.rewindDirectory();
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const uint32_t sz = static_cast<uint32_t>(f.size());
    f.close();
    if (!noteJsonIncludes(name)) continue;
    if (!zipNameIsUsable(name)) continue;   // the writer would refuse it anyway
    zipSizerAdd(&sizer, strlen(name), sz);
  }
  const uint32_t total = zipSizerTotal(&sizer);
  const uint32_t entries = sizer.count;

  server->setContentLength(total);
  server->sendHeader("Content-Disposition",
                     "attachment; filename=\"ardosia-notes.zip\"");
  server->send(200, "application/zip", "");

  ZipHttpSink sink;
  ZipWriter w;
  zipInit(&w, zipHttpSink, &sink);

  char path[320];
  uint8_t chunk[512];

  // --- walk 2: local headers and data -------------------------------------
  dir.rewindDirectory();
  for (auto f = dir.openNextFile(); f && zipOk(&w); f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const uint32_t sz = static_cast<uint32_t>(f.size());
    f.close();
    if (!noteJsonIncludes(name) || !zipNameIsUsable(name)) continue;

    snprintf(path, sizeof(path), "/notes/%s", name);
    uint32_t crc = 0;
    if (!zipCrcOfNote(path, &crc)) continue;

    zipLocalHeader(&w, name, sz, crc, ZIP_NO_DATE, ZIP_NO_TIME);

    auto src = SdMan.open(path, O_RDONLY);
    if (!src) break;   // the length is already promised; nothing to do but stop
    uint32_t sent = 0;
    while (src.available() && sent < sz && zipOk(&w)) {
      const int n = src.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      const uint32_t want = (sz - sent) < static_cast<uint32_t>(n)
                                ? (sz - sent) : static_cast<uint32_t>(n);
      zipData(&w, chunk, want);
      sent += want;
    }
    src.close();
  }

  // --- walk 3: the central directory, offsets recomputed ------------------
  zipStartCentral(&w);
  uint32_t off = 0;
  dir.rewindDirectory();
  for (auto f = dir.openNextFile(); f && zipOk(&w); f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const uint32_t sz = static_cast<uint32_t>(f.size());
    f.close();
    if (!noteJsonIncludes(name) || !zipNameIsUsable(name)) continue;

    snprintf(path, sizeof(path), "/notes/%s", name);
    uint32_t crc = 0;
    if (!zipCrcOfNote(path, &crc)) continue;

    zipCentralHeader(&w, name, sz, crc, ZIP_NO_DATE, ZIP_NO_TIME, off);
    off += zipLocalEntrySize(strlen(name), sz);
  }
  dir.close();

  zipFinish(&w);
  zipHttpFlush(&sink);

  // One counter bump and ONE refresh, not one per note: the panel costs about a
  // second a refresh, and the ghost-press rounds established what a refresh
  // storm does to this device.
  if (zipOk(&w) && sink.ok) {
    filesSent += entries;
    screenDirty = true;
    DBG_PRINTF("[SYNC] Exported %u notes, %u bytes\n", (unsigned)entries,
               (unsigned)total);
  } else {
    DBG_PRINTLN("[SYNC] Export aborted mid-stream");
  }
}

// =========================================================================
// GET / — the notes page
// =========================================================================
//
// Rendered on the device and streamed, not fetched by script. The markup lives
// in src/web_page.h and the per-row formatting in src/note_html.* (pure, host
// tested); everything here is the SD half.
//
// TWO WALKS OF THE FOLDER:
//
//   1. names only, to count — because the count is printed in the header,
//      above the list, and a chunked response cannot go back and fill it in.
//      No file is opened for reading, so this is a directory read and nothing
//      more.
//   2. the rows.
//
// The count uses noteJsonIncludes(), the SAME predicate as the list, rather
// than countNoteFiles(): that one compares the extension case-insensitively,
// so a "NOTES.TXT" dropped on the card by hand would be counted and then not
// listed. "12 notes" above eleven rows is a bug report waiting to happen.
//
// Both walks see the same folder for the same reason the export's three do: one
// handleClient() call on the single Arduino loop task, no POST in between, and
// the autosave structurally excluded while the screen is UIState::WIFI_SYNC.
//
// Entry order is FAT directory order, which in practice is close to creation
// order on a card where notes are added and rarely deleted. Honouring the
// device's Note Order setting would mean holding every name in RAM to sort
// them — exactly the shape P0 took OUT of handleFileList(), for a list the user
// can sort in their file manager after unzipping.

// sendContent(const char*) does not exist: the only single-argument overload
// takes a String, so a literal would be COPIED to the heap on its way out —
// 1.5 KB of it for the page head, on the heap this feature is already tight on.
// The array reference also means this cannot be handed a pointer by accident.
template <size_t N>
static void sendLiteral(const char (&lit)[N]) {
  server->sendContent(lit, N - 1);
}

static void handleIndex() {
  lastHttpActivityMs = millis();
  if (!pcConnected) {
    pcConnected = true;
    screenDirty = true;
  }

  char name[256];

  // --- walk 1: the count --------------------------------------------------
  int count = 0;
  auto dir = SdMan.open("/notes");
  const bool folderOk = dir && dir.isDirectory();
  if (folderOk) {
    dir.rewindDirectory();
    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      f.getName(name, sizeof(name));
      f.close();
      if (noteJsonIncludes(name)) count++;
    }
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "text/html; charset=utf-8", "");
  sendLiteral(WEB_PAGE_HEAD);

  char head[80];
  const int headLen = snprintf(head, sizeof(head), WEB_PAGE_COUNT_FMT, count,
                               count == 1 ? "" : "s");
  if (headLen > 0) server->sendContent(head, (size_t)headLen);

  if (!folderOk || count == 0) {
    if (dir) dir.close();
    if (folderOk) sendLiteral(WEB_PAGE_EMPTY);
    else          sendLiteral(WEB_PAGE_NO_CARD);
    sendLiteral(WEB_PAGE_TAIL);
    server->sendContent("");
    return;
  }

  sendLiteral(WEB_PAGE_BUTTON);
  sendLiteral(WEB_PAGE_LIST_OPEN);

  // --- walk 2: the rows ---------------------------------------------------
  char title[MAX_TITLE_LEN];
  char row[NOTE_HTML_ROW_MAX];

  dir.rewindDirectory();
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const unsigned long sz = (unsigned long)f.size();
    f.close();
    if (!noteJsonIncludes(name)) continue;

    // The same title the device's own list shows, from the same function —
    // including the accent it cannot get back, because the title is not stored
    // anywhere but in the filename (SPEC §3.2). Showing the raw filename
    // instead would be a different lie, and a less readable one.
    filenameToTitle(name, title, MAX_TITLE_LEN);

    // Zero means the row would not fit, and the contract is to skip it rather
    // than send a partial one: half an <li> takes the rest of the list with
    // it. With NOTE_HTML_ROW_MAX this cannot happen for a name FAT can hold.
    const size_t len = noteHtmlRow(name, title, sz, row, sizeof(row));
    if (len == 0) {
      DBG_PRINTF("[SYNC] Skipped unrepresentable name on page: %s\n", name);
      continue;
    }
    server->sendContent(row, len);
  }
  dir.close();

  sendLiteral(WEB_PAGE_LIST_CLOSE);
  sendLiteral(WEB_PAGE_TAIL);
  server->sendContent("");  // zero-length chunk: ends a chunked response
}

static void handleSyncComplete() {
  lastHttpActivityMs = millis();
  server->send(200, "text/plain", "OK");
  DBG_PRINTLN("[SYNC] PC signaled sync complete");
  syncCompletePending = true;  // enterDoneState() called from wifiSyncLoop, not here
}

static void handleNotFound() {
  String uri = server->uri();

  if (uri.startsWith("/notes/") && uri.length() > 7 && server->method() == HTTP_GET) {
    handleFileDownload();
    return;
  }

  server->send(404, "text/plain", "Not found");
}

// POST /notes?title=<url-encoded>&mode=append
//
// canRaw MUST stay false. Arduino-ESP32's raw path (Parsing.cpp:176-197)
// streams the body and never calls _parseArguments, so ?title= is dropped
// and the handler only ever sees "missing title". The plain-POST path
// parses the query first, then exposes the body as arg("plain"). A clipping
// is at most CLIPPING_TEXT_MAX (4 KB) on the X3; NOTE_POST_MAX_BODY is the
// cap. server->on() is not used: its fourth argument is multipart upload.
class NotesPostHandler : public RequestHandler {
 public:
  bool canHandle(HTTPMethod method, String uri) override {
    return method == HTTP_POST && uri == "/notes";
  }

  bool handle(WebServer& srv, HTTPMethod /*method*/, String /*uri*/) override {
      lastHttpActivityMs = millis();
    if (!pcConnected) {
      pcConnected = true;
      screenDirty = true;
    }

    auto sendErr = [&](int code, const char* msg) {
      srv.send(code, "text/plain", msg);
      return true;
    };

    // R1.7 — the hotspot, and only the hotspot. SYNCING is STA mode: the device
    // sits on a LAN whose admission it does not control, so anyone already on
    // that network could write notes onto the card. On the soft AP the device
    // itself decides who is on the network, because it holds the passphrase.
    // Revisit together with authentication for STA Sync, not before.
    if (syncState != SyncState::AP_ACTIVE) {
      return sendErr(403, "forbidden");
    }
    if (!srv.hasArg("title")) {
      return sendErr(400, "missing title");
    }

    const String title = srv.arg("title");
    if (!utf8Validate(title.c_str(), title.length())) {
      return sendErr(400, "invalid utf-8 title");
    }
    if (title.indexOf("..") >= 0 || title.indexOf('/') >= 0 || title.indexOf('\\') >= 0) {
      return sendErr(400, "illegal title");
    }

    const String body = srv.arg("plain");
    if (body.length() > NOTE_POST_MAX_BODY) {
      return sendErr(413, "body too large");
    }

    const bool replace = srv.hasArg("mode") && srv.arg("mode") == "replace";
    char chosenName[MAX_FILENAME_LEN];
    if (!resolveNoteFilename(title.c_str(), replace, chosenName, MAX_FILENAME_LEN)) {
      return sendErr(507, "too many files for this title");
    }
    if (!noteFilenameIsSafe(chosenName)) {
      return sendErr(400, "illegal filename");
    }

    char path[320];
    snprintf(path, sizeof(path), "/notes/%s", chosenName);
    if (strncmp(path, "/notes/", 7) != 0 || strstr(path, "..") != nullptr) {
      return sendErr(400, "illegal path");
    }

    const bool creating = !SdMan.exists(path);
    if (creating && countNoteFiles() >= MAX_FILES) {
      return sendErr(507, "too many notes");
    }

    // Peek the tail before opening for write: SdFat allows one handle
    // per path on hardware.
    int prefixN = 0;
    if (!replace && body.length() > 0 && !creating) {
      char tail[2] = {0, 0};
      size_t tailLen = 0;
      auto peek = SdMan.open(path, O_RDONLY);
      if (peek) {
        const uint32_t szPeek = peek.size();
        if (szPeek >= 2 && peek.seek(szPeek - 2)) {
          const int n = peek.read(tail, 2);
          tailLen = n > 0 ? (size_t)n : 0;
        } else if (szPeek == 1) {
          const int n = peek.read(tail, 1);
          tailLen = n > 0 ? (size_t)n : 0;
        }
        peek.close();
      }
      prefixN = noteAppendPrefixNewlines(tail, tailLen);
    }

    const oflag_t flags = replace
        ? (O_WRONLY | O_CREAT | O_TRUNC)
        : (O_WRONLY | O_CREAT | O_APPEND);
    auto file = SdMan.open(path, flags);
    if (!file) {
      return sendErr(500, "open failed");
    }

    size_t appended = 0;
    auto writeAll = [&](const char* p, size_t n) -> bool {
      if (n == 0) return true;
      const size_t wrote = file.write(reinterpret_cast<const uint8_t*>(p), n);
      if (wrote != n) return false;
      appended += n;
      return true;
    };
    if (!writeAll("\n\n", (size_t)prefixN) ||
        !writeAll(body.c_str(), body.length())) {
      file.close();
      return sendErr(500, "write failed");
    }
    const unsigned long sz = (unsigned long)file.size();
    file.close();
    if (creating) noteSeqAssignNew(chosenName);

    char json[160];
    snprintf(json, sizeof(json),
             "{\"file\":\"%s\",\"size\":%lu,\"appended\":%u}",
             chosenName, sz, (unsigned)appended);
    srv.send(200, "application/json", json);

    filesReceived++;
    notesRefreshPending = true;
    screenDirty = true;
    return true;
  }
};

static void startHttpServer() {
  if (server) return;
  server = new WebServer(80);
  server->addHandler(new NotesPostHandler());
  server->on("/", HTTP_GET, handleIndex);
  server->on("/api/files", HTTP_GET, handleFileList);
  server->on("/api/export.zip", HTTP_GET, handleExportZip);
  server->on("/api/sync-complete", HTTP_POST, handleSyncComplete);
  server->onNotFound(handleNotFound);
  server->begin();
  // No mDNS. It cost a task stack and its structures to publish
  // "ardosia.local", a name nothing here resolves: the IP is drawn on the
  // hotspot screen and every documented step uses it. Heap is the scarce
  // resource on this device — see CLAUDE.md bug #7 for how scarce.
  DBG_PRINTF("[SYNC] HTTP server started at %s\n", currentSyncIp().c_str());
}

static void stopHttpServer() {
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }

}

// =========================================================================
// Input handling — called from input_handler for all key events
// =========================================================================

void syncHandleKey(uint8_t keyCode, uint8_t modifiers) {
  switch (syncState) {
    case SyncState::SCANNING:
      // No input during scan
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::NETWORK_LIST:
      if (keyCode == HID_KEY_DOWN && networkCount > 0) {
        selectedNet = (selectedNet + 1) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_UP && networkCount > 0) {
        selectedNet = (selectedNet - 1 + networkCount) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_ENTER && networkCount > 0) {
        // Try saved password first
        char savedPass[MAX_PASSWORD_LEN + 1];
        if (getSavedPassword(networks[selectedNet].ssid, savedPass, sizeof(savedPass))) {
          usedSavedPassword = true;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, savedPass);
        } else if (!networks[selectedNet].encrypted) {
          // Open network — connect directly
          usedSavedPassword = false;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, "");
        } else {
          // Need password
          usedSavedPassword = false;
          autoConnectAttempted = false;
          passwordBuf[0] = '\0';
          passwordLen = 0;
          syncState = SyncState::PASSWORD_ENTRY;
          screenDirty = true;
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::PASSWORD_ENTRY:
      if (keyCode == HID_KEY_ENTER) {
        if (passwordLen > 0) {
          beginConnect(networks[selectedNet].ssid, passwordBuf);
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      } else if (keyCode == HID_KEY_BACKSPACE) {
        if (passwordLen > 0) {
          passwordLen--;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      } else {
        // Printable character — reuse hidToAscii from input_handler
        extern char hidToAscii(uint8_t hid, uint8_t modifiers);
        char c = hidToAscii(keyCode, modifiers);
        if (c != 0 && c >= ' ' && c != '\n' && c != '\t' && passwordLen < MAX_PASSWORD_LEN) {
          passwordBuf[passwordLen++] = c;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      }
      break;

    case SyncState::CONNECTING:
      // No input while connecting (timeout handles failure)
      if (keyCode == HID_KEY_ESCAPE) {
        WiFi.disconnect(true);
        if (autoConnectAttempted) {
          // Was auto-connecting — fall back to scan
          beginScan();
        } else {
          syncState = SyncState::NETWORK_LIST;
          screenDirty = true;
        }
      }
      break;

    case SyncState::SYNCING:
    case SyncState::AP_ACTIVE:
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::DONE:
      // Any key press returns to menu immediately
      wifiSyncStop();
      break;

    case SyncState::CONNECT_FAILED:
      if (keyCode == HID_KEY_ENTER) {
        // Back to network list, re-scan
        beginScan();
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Up = Yes (save), Down = No (skip)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        saveCredential(connectingSSID, passwordBuf);
        DBG_PRINTF("[SYNC] Saved credentials for %s\n", connectingSSID);
        enterSyncingState();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        enterSyncingState();
      }
      break;

    case SyncState::FORGET_PROMPT:
      // Up = Yes (forget), Down = No (keep)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        forgetCredential(connectingSSID);
        DBG_PRINTF("[SYNC] Forgot credentials for %s\n", connectingSSID);
        beginScan();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        // Keep credentials — go to network list so user can retry manually
        // rather than triggering auto-connect again immediately
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      }
      break;
  }
}

// =========================================================================
// Public API
// =========================================================================

void wifiSyncStart() {
  if (syncActive) return;
  syncActive = true;
  wifiPrefs.begin("wifi_creds", false);
  resetSyncTracking();

  beginScan();

  DBG_PRINTLN("[SYNC] WiFi sync started");
}

// Raise the soft AP and put the server behind it. One function because the
// Settings row that regenerates the passphrase has to bounce the AP, and a
// second copy of this sequence is a second place for the passphrase to be read
// before it is minted.
//
// Adapted from CrossPointWebServerActivity::startAccessPoint (CrossInk, MIT).
// WPA2 on purpose: this AP accepts writes.
static bool raiseAp() {
  WiFi.mode(WIFI_AP);
  delay(100);

  // ORDER MATTERS: the passphrase is minted here, AFTER WIFI_AP, because
  // esp_random() is only a hardware RNG with the RF subsystem powered.
  apEnsurePassword();
  if (apPassword[0] == '\0') {
    DBG_PRINTLN("[SYNC] No passphrase - refusing to raise an open AP");
    return false;
  }

  // Both of these before softAP(), and both measured — see the comment above
  // apHoldAwake(). setSleep(false) is esp_wifi_set_ps(WIFI_PS_NONE); the lock
  // is what stops the system-wide automatic light sleep that powers the MAC and
  // baseband down underneath it.
  WiFi.setSleep(false);
  apHoldAwake();


  // Retry rather than fail hard. A single softAP() refusal is usually a
  // transient fight over the shared radio, and the old behaviour — straight to
  // "AP Failed" and back to the main menu — made that look like a broken
  // feature instead of a two-second wait.
  for (int attempt = 0; attempt < 4; attempt++) {
    if (!WiFi.softAP(apEnsureSsid(), apPassword, AP_CHANNEL, false, AP_MAX_CONN)) {
      DBG_PRINTF("[SYNC] softAP attempt %d failed, retrying\n", attempt + 1);
      delay(400);
      continue;
    }

    // AFTER softAP(), not before — and getting this backwards cost a round.
    //
    // softAP() is what calls WiFi.enableAP(true) (WiFiAP.cpp:152), and that is
    // what brings the AP netif up. Calling softAPConfig() first configures a
    // netif that does not exist yet, and enableAP() then overwrites it. The
    // symptom was indistinguishable from doing nothing: the panel read
    // "DHCP: on", because the dhcps flag belongs to the default netif and is
    // true either way, while the address never took.
    //
    // Why it has to be called at all: softAP() never touches DHCP. The only
    // call to esp_netif_dhcps_start() in the whole library is inside
    // set_esp_interface_ip() (WiFiGeneric.cpp:212), reached only from
    // softAPConfig() (WiFiAP.cpp:208). Without it the DHCP server is whatever
    // the event-driven netif default left behind — and radioOff() takes the
    // radio to WIFI_OFF between sessions, which tears that netif down.
    //
    // The values are the ESP32 soft-AP defaults, written out. Being explicit is
    // the point: a DHCP server we know is running, not one we hope is.
    if (!WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                           IPAddress(255, 255, 255, 0))) {
      DBG_PRINTLN("[SYNC] softAPConfig failed - there will be no DHCP server");
    }
    delay(100);
    return true;
  }
  return false;
}

void wifiSyncStartAp() {
  if (syncActive) return;
  syncActive = true;
  resetSyncTracking();
  lastSeenStations = -1;

  // Before the AP exists, not after: with a BLE link live the 4-way handshake
  // times out (reason=15) rather than being rejected.
  // 16 KB that the hotspot needs more than a pending paste does. See
  // editorClipboardRelease(). Before the refusal check, deliberately: it is
  // harmless if the AP never comes up, and ordering it after would skip it on
  // exactly the retry path where memory is already tight.
  editorClipboardRelease();

  if (!bleOffTheAirForAp()) {
    strcpy(statusText, "Bluetooth busy - try again");
    syncState = SyncState::DONE;
    doneStartMs = millis();
    screenDirty = true;
    return;
  }

  if (!raiseAp()) {
    strcpy(statusText, "AP failed");
    syncState = SyncState::DONE;
    doneStartMs = millis();
    screenDirty = true;
    DBG_PRINTLN("[SYNC] Failed to start Access Point");
    return;
  }

  syncState = SyncState::AP_ACTIVE;
  startHttpServer();
  snprintf(statusText, sizeof(statusText), "%s", currentSyncIp().c_str());
  apStartMs = millis();
  lastHttpActivityMs = millis();
  screenDirty = true;
  // The SSID, never the passphrase. This log goes to a console nobody can read
  // on the X4, but the binary is public and the habit is what matters.
  DBG_PRINTF("[SYNC] AP \"%s\" up at %s\n", apSsid, statusText);
}

void wifiSyncStop() {
  if (!syncActive) return;

  stopHttpServer();
  radioOff();

  wifiPrefs.end();
  syncActive = false;
  networkCount = 0;
  passwordBuf[0] = '\0';
  passwordLen = 0;
  statusText[0] = '\0';

  // Return to main menu
  extern UIState currentState;
  currentState = UIState::MAIN_MENU;
  screenDirty = true;

  DBG_PRINTLN("[SYNC] WiFi sync stopped");
}

void wifiSyncLoop() {
  if (!syncActive) return;

  switch (syncState) {
    case SyncState::SCANNING:
      processScanResults();
      break;

    case SyncState::CONNECTING:
      pollConnection();
      break;

    case SyncState::SYNCING:
      if (server) server->handleClient();
      if (notesRefreshPending) {
        notesRefreshPending = false;
        refreshFileList();
      }
      if (syncCompletePending) {
        syncCompletePending = false;
        enterDoneState();
      } else if (millis() - lastHttpActivityMs > SYNC_TIMEOUT_MS) {
        DBG_PRINTLN("[SYNC] Timeout — no HTTP activity for 60s");
        enterDoneState();
      }
      break;

    case SyncState::AP_ACTIVE:
      if (server) server->handleClient();
      if (notesRefreshPending) {
        notesRefreshPending = false;
        refreshFileList();
      }
      {
        const int stations = (int)WiFi.softAPgetStationNum();
        if (stations != lastSeenStations) {
          if (lastSeenStations == 0 && stations > 0) {
            // Grace period: the X3 takes a while to join; don't start the
            // 60 s idle clock until a station is actually associated.
            lastHttpActivityMs = millis();
          }
          lastSeenStations = stations;
          screenDirty = true;
        }
        if (stations == 0) {
          if (millis() - apStartMs > AP_NO_CLIENT_MS) {
            DBG_PRINTLN("[SYNC] AP timeout — no client in 5 min");
            enterDoneState();
          }
        } else if (millis() - lastHttpActivityMs > SYNC_TIMEOUT_MS) {
          DBG_PRINTLN("[SYNC] AP idle — no HTTP for 60s");
          enterDoneState();
        }
      }
      break;

    case SyncState::DONE:
      // Auto-return to menu after 3 seconds
      if (millis() - doneStartMs > DONE_DISPLAY_MS) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Server is NOT running during save prompt (will start after user responds)
      break;

    default:
      break;
  }
}

bool isWifiSyncActive() {
  return syncActive;
}

SyncState getSyncState() {
  return syncState;
}

int getNetworkCount() {
  return networkCount;
}

const char* getNetworkSSID(int i) {
  if (i < 0 || i >= networkCount) return "";
  return networks[i].ssid;
}

int getNetworkRSSI(int i) {
  if (i < 0 || i >= networkCount) return -100;
  return networks[i].rssi;
}

bool isNetworkEncrypted(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].encrypted;
}

bool isNetworkSaved(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].saved;
}

int getSelectedNetwork() {
  return selectedNet;
}

const char* getPasswordBuffer() {
  return passwordBuf;
}

int getPasswordLen() {
  return passwordLen;
}

const char* getSyncStatusText() {
  return statusText;
}

int getSyncFilesSent() {
  return filesSent;
}

int getSyncFilesReceived() {
  return filesReceived;
}

bool isPcConnected() {
  return pcConnected;
}

const char* getApSsid() { return apEnsureSsid(); }

// Empty until the hotspot has been raised once. The hotspot screen is the only
// caller and it only draws while AP_ACTIVE, by which point raiseAp() has run.
const char* getApPassword() { return apPassword; }

// R1.5 — Settings -> System -> Hotspot Password -> Regenerate.
//
// The stored key is dropped first and unconditionally, so the old passphrase
// stops working the moment the user confirms, whatever happens next.
//
// The NEW one is minted only with the radio up, which is the same rule
// raiseAp() follows and the reason this is two branches rather than one:
//
//   hotspot running   RF is on. Mint now and bounce the AP, which drops every
//                     client that joined with the old passphrase.
//   hotspot down      RF is off, and minting here would draw from a PRNG. Leave
//                     it empty; the next wifiSyncStartAp() mints it after
//                     WIFI_AP, and that is the first moment anyone could use it
//                     anyway.
void hotspotRegeneratePassword() {
  Preferences apPrefs;
  if (apPrefs.begin(AP_NVS_NS, false)) {
    apPrefs.remove(AP_NVS_KEY);
    apPrefs.end();
  }
  apPassword[0] = '\0';

  if (!syncActive || syncState != SyncState::AP_ACTIVE) {
    DBG_PRINTLN("[SYNC] Hotspot passphrase cleared - a new one is minted at next start");
    return;
  }

  // Take the server down first: the AP netif goes away under it.
  stopHttpServer();
  WiFi.softAPdisconnect(true);
  delay(100);

  if (!raiseAp()) {
    strcpy(statusText, "AP failed");
    syncState = SyncState::DONE;
    doneStartMs = millis();
    screenDirty = true;
    DBG_PRINTLN("[SYNC] Failed to restart AP after regenerating the passphrase");
    return;
  }

  startHttpServer();
  snprintf(statusText, sizeof(statusText), "%s", currentSyncIp().c_str());
  apStartMs = millis();
  lastHttpActivityMs = millis();
  lastSeenStations = -1;
  screenDirty = true;
  DBG_PRINTLN("[SYNC] Hotspot passphrase regenerated, AP restarted");
}

int getApStationCount() {
  if (syncState != SyncState::AP_ACTIVE) return 0;
  return (int)WiFi.softAPgetStationNum();
}

bool wifiSyncUiChanged() {
  static int lastSent = -1, lastRecv = -1, lastStations = -1;
  static bool lastPc = false;
  static SyncState lastState = SyncState::SCANNING;
  const int stations = getApStationCount();
  const bool changed = (filesSent != lastSent)
                    || (filesReceived != lastRecv)
                    || (stations != lastStations)
                    || (pcConnected != lastPc)
                    || (syncState != lastState);
  lastSent = filesSent;
  lastRecv = filesReceived;
  lastStations = stations;
  lastPc = pcConnected;
  lastState = syncState;
  return changed;
}
