#pragma once

// HID usage code -> character, per keyboard layout.
//
// The critical fact: a BLE keyboard reports keys by PHYSICAL POSITION, never by
// what is printed on the keycap. Usage 0x34 means "the second key right of L"
// on every keyboard on earth — it is ' on US ANSI and ~ on ABNT2. So the layout
// is not something the device can detect; it has to be told.
//
// Kept free of Arduino so test/run.sh can exercise it on a host compiler.

#include "config.h"
#include <cstdint>

// Which accent a dead key arms. None for ordinary keys.
enum class DeadMark : uint8_t { None, Acute, Grave, Circumflex, Tilde, Diaeresis };

struct KeyStroke {
  uint32_t cp;    // codepoint this key produces; 0 when it prints nothing
  DeadMark mark;  // non-None when this key is a dead accent in this layout
};

// Resolve one key press. `caps` is the CapsLock state, which combines with
// Shift for letters (and for ç on ABNT2) but not for symbols.
KeyStroke keymapResolve(KeyboardLayout layout, uint8_t hid, uint8_t modifiers, bool caps);
