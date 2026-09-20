#pragma once

// Accent composition: the dead-key state machine.
//
// A dead key prints nothing and arms an accent; the next key decides what comes
// out.  ´ then a -> á.  ~ then a -> ã.  ´ then space -> ´.  ´ then t -> ´t.
//
// Which keys are dead, and which mark each one carries, is the layout's job
// (src/keymap.cpp).  This file only composes, so ABNT2 and US-International
// share it unchanged even though they put the marks on different keys.
//
// Deliberately free of Arduino and of input_handler state: on a USB-locked
// device there is no serial console, so logic that cannot be exercised on a
// host compiler cannot be exercised at all.  test/test_ptbr.cpp compiles it.

#include "keymap.h"
#include <cstdint>

// Compose an accent onto a base character, or 0 if the pair has no accented
// form. `base` may be upper or lower case; the result follows.
uint32_t deadKeyCompose(DeadMark mark, uint32_t base);

// Feed one resolved key press through the state machine.
// Writes 0..2 codepoints into out[] and returns how many:
//   0  an accent was armed; nothing prints yet
//   1  an ordinary character, or a composed one
//   2  the accent had no composed form, so accent + character are both emitted
int deadKeyFeed(const KeyStroke& key, uint32_t out[2]);

// The armed accent's own character (´ ~ ^ ...), or 0 when nothing is pending.
// The editor draws it inside the cursor so the wait is visible on slow e-ink.
uint32_t deadKeyPending();
void deadKeyReset();
