#pragma once

#include <stddef.h>

#include <cstdint>

// Sync state machine states (exposed as int via getSyncState())
enum class SyncState : uint8_t {
  SCANNING,
  NETWORK_LIST,
  PASSWORD_ENTRY,
  CONNECTING,
  SYNCING,         // Server running, tracking transfers
  AP_ACTIVE,       // SoftAP + server; same HTTP as SYNCING, no STA
  DONE,            // Summary shown, WiFi off, auto-return to menu
  CONNECT_FAILED,
  SAVE_PROMPT,
  FORGET_PROMPT
};

// One-time cleanup of the Wi-Fi credential backup this firmware no longer
// writes. Call once on boot, after the card is mounted. See F3 in
// SPEC-release-blockers.md: the file held the passwords of the user's OTHER
// networks in plaintext on removable media.
void wifiSyncMigrateDropSdBackup();

// Throw away the hotspot passphrase and mint a new one. Wired to
// Settings -> System -> Hotspot Password. The old passphrase stops working
// immediately; if the hotspot is up, the AP is restarted so clients that saved
// the old one are dropped. Safe to call with the radio down.
void hotspotRegeneratePassword();

// Lifecycle
void wifiSyncStart();       // Begin scanning (or auto-connect if saved creds)
void wifiSyncStartAp();     // Raise the Ardosia hotspot and start the server
void wifiSyncStop();         // Stop everything, WiFi off
void wifiSyncLoop();         // Poll scan/connection/HTTP
bool isWifiSyncActive();
// True when a counter or state the sync screen shows has changed since
// the last call. Used to skip the 2 s e-ink refresh when nothing moved.
bool wifiSyncUiChanged();

// For UI renderer
SyncState getSyncState();
int  getNetworkCount();
const char* getNetworkSSID(int i);
int  getNetworkRSSI(int i);
bool isNetworkEncrypted(int i);
bool isNetworkSaved(int i);
int  getSelectedNetwork();
const char* getPasswordBuffer();
int  getPasswordLen();
const char* getSyncStatusText();

// Sync progress
int  getSyncFilesSent();
int  getSyncFilesReceived();
int  getSyncTotalFiles();
bool isPcConnected();
// "Ardosia-XXXX", derived from the soft-AP MAC. Available before the AP is up.
const char* getApSsid();
// The per-device passphrase. Empty until the hotspot has been raised once,
// because it is only ever minted with the radio powered.
const char* getApPassword();
int  getApStationCount();

// For input handler
void syncHandleKey(uint8_t keyCode, uint8_t modifiers);

// Whether the soft AP's DHCP server is running, as text for the hotspot screen.
// See the comment on the definition: this exists because "associates but never
// gets an address" was diagnosed by guessing three times, and a fact on the
// panel ends that.
bool apDhcpStatusText(char* out, size_t n);

// Free heap and largest free block, drawn on the hotspot screen while the
// export feature is in progress. The margin there is thin and running out is
// silent. TEMPORARY.
void apHttpStatusText(char* out, size_t n);
