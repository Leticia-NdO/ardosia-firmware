#pragma once

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
const char* getApSsid();
const char* getApPassword();
int  getApStationCount();

// For input handler
void syncHandleKey(uint8_t keyCode, uint8_t modifiers);
