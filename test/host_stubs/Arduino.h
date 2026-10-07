#pragma once

// Host-test stand-in. file_manager.cpp includes <Arduino.h> for the firmware
// DBG macros; with -DRELEASE_BUILD those expand to nothing, so this only
// needs the C library bits snprintf would otherwise get from Arduino.
#include <cstdio>
#include <cstring>
#include <cstdint>

// --- Added for test_buttons.cpp -------------------------------------------
// InputManager needs a clock it can be lied to about, and the two digital
// calls it makes for the power button. hostSetMillis() lets a test step time
// in whatever increments it wants, which is how the debounce gets exercised
// at both poll cadences (10 ms active, 50 ms idle) without sleeping.
#ifdef ARDOSIA_HOST_INPUT
inline unsigned long& hostMillisRef() { static unsigned long t = 0; return t; }
inline void hostSetMillis(const unsigned long t) { hostMillisRef() = t; }
inline void hostAdvanceMillis(const unsigned long d) { hostMillisRef() += d; }
inline unsigned long millis() { return hostMillisRef(); }

#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
inline void pinMode(int, int) {}
inline int& hostPowerPinRef() { static int v = HIGH; return v; }
inline void hostSetPowerPin(const int level) { hostPowerPinRef() = level; }
inline int digitalRead(int) { return hostPowerPinRef(); }
#endif
