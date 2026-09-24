#pragma once

#include "config.h"
#include <cstddef>

void inputSetup();
void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed);
int processAllInput();

// Raw base-layout mapping: HID code + modifiers -> ASCII, 0 if not printable.
// No dead-key processing — callers that want plain ASCII (the WiFi password
// field) use this directly.
char hidToAscii(uint8_t hid, uint8_t modifiers);

// Full text resolution, including the US-International dead-key state machine.
// Writes 0..2 codepoints into out[] and returns how many were written:
//   0  the key armed an accent; nothing is printed yet
//   1  an ordinary or composed character
//   2  the accent had no composed form, so accent + letter are both emitted
int inputResolveText(uint8_t hid, uint8_t modifiers, uint32_t out[2]);

// The raw HID usage code and modifier byte of the last key press that reached
// the UI. Shown in the Settings footer: HID codes are positional, so this is
// how you tell "the keyboard is not US ANSI" apart from "the mapping is wrong".
void inputGetLastKey(uint8_t* hid, uint8_t* mods);

// One-line description of that key for the Settings footer, e.g.
// "Key: 0x34 mod:0x00 -> ~ (dead)".
void inputDescribeLastKey(char* buf, size_t n);

// The accent currently armed and waiting for a letter, or 0. The editor draws
// it inside the cursor so the pending state is visible on a slow e-ink screen.
uint32_t inputGetPendingDeadKey();
void inputClearDeadKey();

// Header for the title-edit screen: it edits a note title or an OTA app's menu
// entry, and says which.
const char* renameScreenTitle();
bool renameTargetIsApp();
