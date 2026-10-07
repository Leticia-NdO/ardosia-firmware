#pragma once

#include <cstddef>
#include <cstdint>

// The hotspot passphrase, minted per device.
//
// WHY THIS EXISTS: the soft AP used to be raised with AP_PASSWORD = "ardosia12",
// the same eleven bytes on every unit and published in a public repository. A
// WPA2 network whose passphrase is in the README is an open network with extra
// steps, and this one accepts POST /notes — it can be written to, not only read.
//
// Free of Arduino and of NVS on purpose, like keymap/confirm/ui_layout: the RNG
// arrives as an argument so test/run.sh can drive it with a stub and assert the
// format, the alphabet and the rejection path without an ESP32. The NVS side —
// generate once, keep, regenerate on request — lives in wifi_sync.cpp, which is
// where the radio is.

// Symbols the passphrase is drawn from: 31 of them. Lowercase only and with
// 0 o 1 l i left out, because this string is read off an e-ink panel and typed
// back in on a phone keyboard or on the X3, where an l that is really a 1 costs
// the user a failed join they cannot diagnose.
extern const char HOTSPOT_PW_ALPHABET[];
static constexpr int HOTSPOT_PW_ALPHABET_N = 31;

// 10 drawn symbols, shown and used as "xxxxx-xxxxx". The hyphen is part of the
// passphrase, not punctuation in the display: 11 characters, which WPA2 accepts
// (8..63). 31^10 is about 49.5 bits, which is the number that matters — WPA2's
// weakness is offline cracking of a captured handshake, and 49.5 bits is far
// out of reach of that for a target of this kind.
static constexpr int HOTSPOT_PW_SYMBOLS = 10;

// "xxxxx-xxxxx" is 11 characters; the buffer holds the terminator too.
static constexpr size_t HOTSPOT_PW_LEN    = 11;
static constexpr size_t HOTSPOT_PW_BUFLEN = HOTSPOT_PW_LEN + 1;

// Writes "xxxxx-xxxxx\0" into `out`. `outLen` must be at least
// HOTSPOT_PW_BUFLEN; on anything smaller, or a null argument, `out` is left as
// the empty string rather than half a passphrase — a caller that ignores that
// and raises an AP gets a rejected WPA2 key, not a guessable one.
//
// `rng` is called once per byte drawn and only its low 8 bits are used.
void hotspotGeneratePassword(char* out, size_t outLen, uint32_t (*rng)());
