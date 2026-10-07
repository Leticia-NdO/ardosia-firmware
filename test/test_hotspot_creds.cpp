// Host-side tests for the hotspot passphrase generator.
//
// The thing being checked is not "does it return a string" but the three
// properties the threat model leans on: the shape is what the screen promises
// and WPA2 accepts, every symbol comes from the confusion-free alphabet, and a
// byte that would bias the draw is thrown away rather than folded in.
//
// Run with test/run.sh.

#include "../src/hotspot_creds.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
  checks++;
  if (!ok) {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static void checkStr(const char* got, const char* want, const char* what) {
  checks++;
  if (strcmp(got, want) != 0) {
    failures++;
    printf("  FAIL  %s\n        got  \"%s\"\n        want \"%s\"\n", what, got, want);
  }
}

// --- Stub RNGs -------------------------------------------------------------
//
// hotspotGeneratePassword takes the RNG as an argument precisely so this file
// can decide what the "hardware" returns. A function pointer cannot capture, so
// the sequence lives in a file-scope cursor.

static const uint32_t* seq = nullptr;
static int seqLen = 0;
static int seqPos = 0;
static int seqCalls = 0;

static void feed(const uint32_t* values, int n) {
  seq = values;
  seqLen = n;
  seqPos = 0;
  seqCalls = 0;
}

// Walks the sequence and wraps, so a short list can drive a whole passphrase.
static uint32_t stubRng() {
  seqCalls++;
  const uint32_t v = seq[seqPos];
  seqPos = (seqPos + 1) % seqLen;
  return v;
}

// ---------------------------------------------------------------------------

static void testFormat() {
  printf("hotspot passphrase: the shape the screen promises\n");

  static const uint32_t ones[] = {0};
  feed(ones, 1);

  char pw[HOTSPOT_PW_BUFLEN];
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);

  check(strlen(pw) == HOTSPOT_PW_LEN, "eleven characters");
  check(strlen(pw) >= 8 && strlen(pw) <= 63, "and inside what WPA2 accepts");
  check(pw[5] == '-', "the hyphen is the sixth character");

  int hyphens = 0;
  for (size_t i = 0; i < strlen(pw); i++) {
    if (pw[i] == '-') hyphens++;
  }
  check(hyphens == 1, "and the only one");

  // Five, hyphen, five.
  const char* dash = strchr(pw, '-');
  check(dash != nullptr && (dash - pw) == 5, "five symbols before it");
  check(dash != nullptr && strlen(dash + 1) == 5, "and five after");
}

static void testAlphabetOnly() {
  printf("hotspot passphrase: nothing outside the alphabet\n");

  check((int)strlen(HOTSPOT_PW_ALPHABET) == HOTSPOT_PW_ALPHABET_N,
        "the alphabet is the length the header claims");

  // The whole reason for a custom alphabet: these six are the characters a
  // reader confuses on an e-ink panel, and none of them may ever be drawn.
  for (const char* bad = "01oliI"; *bad; bad++) {
    check(strchr(HOTSPOT_PW_ALPHABET, *bad) == nullptr,
          "no confusable character is in the alphabet");
  }
  for (const char* c = HOTSPOT_PW_ALPHABET; *c; c++) {
    check((*c >= 'a' && *c <= 'z') || (*c >= '2' && *c <= '9'),
          "the alphabet is lowercase letters and digits 2-9 only");
  }

  // Every byte value 0..255 through the generator: whatever the RNG says, the
  // output stays inside the alphabet. 0xFF included, which is a rejected draw.
  for (uint32_t v = 0; v < 256; v++) {
    const uint32_t one[] = {v};
    feed(one, 1);
    char pw[HOTSPOT_PW_BUFLEN];
    hotspotGeneratePassword(pw, sizeof(pw), stubRng);
    bool ok = strlen(pw) == HOTSPOT_PW_LEN;
    for (size_t i = 0; i < strlen(pw) && ok; i++) {
      if (pw[i] == '-') continue;
      if (strchr(HOTSPOT_PW_ALPHABET, pw[i]) == nullptr) ok = false;
    }
    check(ok, "every RNG byte produces a passphrase of alphabet symbols");
  }

  // Only the low 8 bits are used, so the high bytes cannot leak a symbol in.
  static const uint32_t high[] = {0xFFFFFF00u};
  feed(high, 1);
  char pw[HOTSPOT_PW_BUFLEN];
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);
  checkStr(pw, "22222-22222", "the high 24 bits are ignored");
}

static void testRejectionPath() {
  printf("hotspot passphrase: a biasing byte is thrown away, not folded in\n");

  // 248 is 8 x 31, the first value that must be rejected. 255 is the one the
  // spec names. Both are followed by a 0, so a generator that rejected
  // correctly draws twice per symbol and spells "22222-22222"; one that folded
  // the byte in with % 31 would put 255 % 31 = 7 -> '9' in every slot.
  static const uint32_t rejectThenZero[] = {255, 0};
  feed(rejectThenZero, 2);
  char pw[HOTSPOT_PW_BUFLEN];
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);
  checkStr(pw, "22222-22222", "255 is rejected and redrawn");
  check(seqCalls == 2 * HOTSPOT_PW_SYMBOLS, "which costs one extra draw per symbol");

  static const uint32_t boundary[] = {248, 0};
  feed(boundary, 2);
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);
  checkStr(pw, "22222-22222", "248, the first biasing value, is rejected too");

  // 247 is the last value that must be ACCEPTED. Rejecting it would throw away
  // a legitimate draw and quietly shrink the alphabet's tail.
  static const uint32_t lastGood[] = {247};
  feed(lastGood, 1);
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);
  check(seqCalls == HOTSPOT_PW_SYMBOLS, "247 is accepted, one draw per symbol");
  checkStr(pw, "zzzzz-zzzzz", "and maps to the last symbol of the alphabet");

  // An RNG jammed above the rejection line: the cap has to break the loop or
  // the AP never comes up. What it returns is biased and does not matter; that
  // it returns at all does.
  static const uint32_t jammed[] = {255};
  feed(jammed, 1);
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);
  check(strlen(pw) == HOTSPOT_PW_LEN, "a jammed RNG still terminates");
}

static void testDeterministicForAFixedSequence() {
  printf("hotspot passphrase: a fixed RNG sequence gives a fixed passphrase\n");

  // Indices 0,1,2,...,9 into the alphabet, so the expected string can be read
  // straight off the header's alphabet: "23456789ab".
  static const uint32_t counted[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
  feed(counted, 10);
  char pw[HOTSPOT_PW_BUFLEN];
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);
  checkStr(pw, "23456-789ab", "the drawn bytes index the alphabet in order");

  // Same sequence, same answer: nothing hidden carries over between calls.
  feed(counted, 10);
  char again[HOTSPOT_PW_BUFLEN];
  hotspotGeneratePassword(again, sizeof(again), stubRng);
  checkStr(again, pw, "and the call is repeatable");

  // The modulo wraps at 31: byte 31 is the alphabet's first symbol again.
  static const uint32_t wrap[] = {31};
  feed(wrap, 1);
  hotspotGeneratePassword(pw, sizeof(pw), stubRng);
  checkStr(pw, "22222-22222", "byte 31 wraps to the first symbol");
}

static void testRefusesToHalfFillABuffer() {
  printf("hotspot passphrase: a buffer too small gets nothing, not half\n");

  static const uint32_t ones[] = {0};

  // A truncated passphrase raised as a WPA2 key is the failure this guards: it
  // would be shorter than the panel shows and shorter than 8 characters.
  for (size_t n = 1; n < HOTSPOT_PW_BUFLEN; n++) {
    char small[HOTSPOT_PW_BUFLEN] = "sentinel!!";
    feed(ones, 1);
    hotspotGeneratePassword(small, n, stubRng);
    check(small[0] == '\0', "a short buffer comes back empty");
  }

  // Exactly HOTSPOT_PW_BUFLEN is enough, and nothing is written past it.
  char exact[HOTSPOT_PW_BUFLEN + 1];
  memset(exact, '#', sizeof(exact));
  feed(ones, 1);
  hotspotGeneratePassword(exact, HOTSPOT_PW_BUFLEN, stubRng);
  check(strlen(exact) == HOTSPOT_PW_LEN, "the exact buffer size is enough");
  check(exact[HOTSPOT_PW_BUFLEN] == '#', "and nothing is written past it");

  // A null RNG or a null buffer: no crash, no passphrase.
  char buf[HOTSPOT_PW_BUFLEN] = "sentinel!!";
  hotspotGeneratePassword(buf, sizeof(buf), nullptr);
  check(buf[0] == '\0', "a null RNG comes back empty");
  hotspotGeneratePassword(nullptr, HOTSPOT_PW_BUFLEN, stubRng);
  check(true, "a null buffer does not crash");
}

int main() {
  testFormat();
  testAlphabetOnly();
  testRejectionPath();
  testDeterministicForAFixedSequence();
  testRefusesToHalfFillABuffer();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
