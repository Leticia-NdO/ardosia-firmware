#pragma once

#include "config.h"
#include <string>

// Device info structure for discovered devices
struct BleDeviceInfo {
  std::string address;
  std::string name;
  int rssi;
  uint8_t addressType;    // BLE address type (public/random)
  uint32_t lastSeenMs;    // from millis()
  bool isHid;             // advertises the HID-over-GATT service (0x1812)
  uint16_t appearance;    // GAP appearance; 0x03C1 = Keyboard, 0 = not advertised
};

// GAP appearance values worth naming (Bluetooth Assigned Numbers).
static constexpr uint16_t BLE_APPEARANCE_KEYBOARD = 0x03C1;
static constexpr uint16_t BLE_APPEARANCE_MOUSE    = 0x03C2;
// The HID category is the top 10 bits: 0x03C0 >> 6 == 0x0F.
static constexpr uint16_t BLE_APPEARANCE_CAT_HID  = 0x0F;

void bleSetup();
void bleLoop();
bool isKeyboardConnected();
BLEState getConnectionState();

// Functions for Bluetooth device management
void startDeviceScan();
void stopDeviceScan();
int getDiscoveredDeviceCount();
BleDeviceInfo* getDiscoveredDevices();
void connectToDevice(int deviceIndex);
void disconnectCurrentDevice();
std::string getCurrentDeviceAddress();

// Global flag to control auto-reconnect behavior
extern bool autoReconnectEnabled;

// Max keyboards that can be stored
static constexpr int MAX_PAIRED_KEYBOARDS = 4;

// Multi-keyboard storage API
int  getPairedKeyboardCount();
bool getPairedKeyboard(int index, std::string& addr, std::string& name, uint8_t& addrType);
int  getLastUsedKeyboardIndex();   // -1 if none stored
bool removePairedKeyboard(int index);
void connectToPairedKeyboard(int index);

// Internal helpers (also used by ui_renderer for backwards compat)
void storePairedDevice(const std::string& address, const std::string& name);
bool getStoredDevice(std::string& address, std::string& name);
void clearStoredDevice();

// Function for getting current passkey for UI display
uint32_t getCurrentPasskey();

// Bluetooth scanning status functions
bool isDeviceScanning();
uint32_t getScanAgeMs();
void refreshScanNow();
void clearAllBluetoothBonds();
void cancelPendingConnection();

// True while the BLE stack has the shared 2.4 GHz radio: a connect task is in
// flight, or a scan is running.
//
// Exists because cancelPendingConnection() is NOT synchronous — its own comment
// says "Can't safely kill the task mid-connection, but prevent new attempts".
// A caller that needs the radio has to WAIT for this to go false, not merely
// ask for the cancel. Raising the soft AP on top of a live connect task is what
// made WiFi.softAP() fail outright right after boot, when reconnectDelay is
// still 5 s and a connect task fires every few seconds.
bool bleRadioBusy();

// Empty while BLE is healthy; otherwise a short reason the keyboard is not
// connecting that is not about the keyboard — today, a failed CONNECT_STACK
// contiguous allocation for the connect task, which is what a fragmented heap
// after a hotspot session does. For the Bluetooth screen, because that is
// where someone goes when the keyboard stops working.
const char* bleNoteText();
