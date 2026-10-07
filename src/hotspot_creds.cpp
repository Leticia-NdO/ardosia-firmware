#include "hotspot_creds.h"

const char HOTSPOT_PW_ALPHABET[] = "23456789abcdefghjkmnpqrstuvwxyz";

// 8 x 31. A byte at or above this is thrown away rather than folded into the
// alphabet: 256 is not a multiple of 31, so taking every byte modulo 31 would
// make the first 8 symbols of the alphabet turn up 9 times out of 256 and the
// rest 8 times. That bias is small but it is free to remove, and an attacker
// guessing passphrases in most-likely-first order is exactly who would collect
// it.
static constexpr unsigned REJECT_AT = 8u * (unsigned)HOTSPOT_PW_ALPHABET_N;

// Rejection sampling has no upper bound in theory. In practice 8 of every 256
// bytes are rejected, so 64 in a row is a probability of (8/256)^64 — it does
// not happen. What it guards against is not chance but a broken RNG that
// returns a constant above 247, which on a device with no console would be an
// AP that never comes up at all. Past the cap the byte is folded in anyway:
// a very slightly biased passphrase beats a hotspot that hangs.
static constexpr int MAX_DRAWS_PER_SYMBOL = 64;

void hotspotGeneratePassword(char* out, size_t outLen, uint32_t (*rng)()) {
  if (out == nullptr || outLen == 0) return;
  out[0] = '\0';
  if (outLen < HOTSPOT_PW_BUFLEN || rng == nullptr) return;

  size_t w = 0;
  for (int i = 0; i < HOTSPOT_PW_SYMBOLS; i++) {
    // The hyphen sits in the middle, so the two halves are the same length and
    // the eye can hold five characters at a time off the panel.
    if (i == HOTSPOT_PW_SYMBOLS / 2) out[w++] = '-';

    unsigned b = 0;
    for (int draw = 0; draw < MAX_DRAWS_PER_SYMBOL; draw++) {
      b = rng() & 0xFFu;
      if (b < REJECT_AT) break;
    }
    out[w++] = HOTSPOT_PW_ALPHABET[b % (unsigned)HOTSPOT_PW_ALPHABET_N];
  }
  out[w] = '\0';
}
