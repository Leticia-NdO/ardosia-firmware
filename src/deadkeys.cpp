#include "deadkeys.h"

// ---------------------------------------------------------------------------
// Composition table.
//
// Keyed on the MARK, not on the key that produced it: ABNT2 arms the acute from
// the ´ key while US-International arms it from the apostrophe, and both land
// here identically.
//
// Uppercase is derived rather than tabulated — in Latin-1 every accented
// capital sits exactly 0x20 below its lowercase form. The one exception is ÿ,
// whose capital is Ÿ (U+0178) in Latin Extended-A, not 0xDF.
// ---------------------------------------------------------------------------

namespace {

struct DeadCombo {
  DeadMark mark;
  char     base;    // lowercase letter that follows the accent
  uint16_t result;  // composed codepoint
};

const DeadCombo COMBOS[] = {
  {DeadMark::Acute,      'a', 0x00E1}, {DeadMark::Acute,      'e', 0x00E9},
  {DeadMark::Acute,      'i', 0x00ED}, {DeadMark::Acute,      'o', 0x00F3},
  {DeadMark::Acute,      'u', 0x00FA}, {DeadMark::Acute,      'y', 0x00FD},
  {DeadMark::Acute,      'c', 0x00E7},   // ´c gives ç on ABNT2 too

  {DeadMark::Grave,      'a', 0x00E0}, {DeadMark::Grave,      'e', 0x00E8},
  {DeadMark::Grave,      'i', 0x00EC}, {DeadMark::Grave,      'o', 0x00F2},
  {DeadMark::Grave,      'u', 0x00F9},

  {DeadMark::Circumflex, 'a', 0x00E2}, {DeadMark::Circumflex, 'e', 0x00EA},
  {DeadMark::Circumflex, 'i', 0x00EE}, {DeadMark::Circumflex, 'o', 0x00F4},
  {DeadMark::Circumflex, 'u', 0x00FB},

  {DeadMark::Tilde,      'a', 0x00E3}, {DeadMark::Tilde,      'o', 0x00F5},
  {DeadMark::Tilde,      'n', 0x00F1},

  {DeadMark::Diaeresis,  'a', 0x00E4}, {DeadMark::Diaeresis,  'e', 0x00EB},
  {DeadMark::Diaeresis,  'i', 0x00EF}, {DeadMark::Diaeresis,  'o', 0x00F6},
  {DeadMark::Diaeresis,  'u', 0x00FC}, {DeadMark::Diaeresis,  'y', 0x00FF},
};

// The accent waiting for its letter.
DeadMark pendingMark    = DeadMark::None;
uint32_t pendingLiteral = 0;   // the accent's own character, for display/fallback

}  // namespace

uint32_t deadKeyCompose(const DeadMark mark, const uint32_t base) {
  if (mark == DeadMark::None || base > 0x7F) return 0;

  const char c = static_cast<char>(base);
  const bool upper = (c >= 'A' && c <= 'Z');
  const char lower = upper ? static_cast<char>(c + 32) : c;

  for (const DeadCombo& combo : COMBOS) {
    if (combo.mark != mark || combo.base != lower) continue;
    if (!upper) return combo.result;
    if (combo.result == 0x00FF) return 0x0178;   // ÿ -> Ÿ, not 0xDF
    return combo.result - 0x20;
  }
  return 0;
}

uint32_t deadKeyPending() { return pendingLiteral; }

void deadKeyReset() {
  pendingMark = DeadMark::None;
  pendingLiteral = 0;
}

int deadKeyFeed(const KeyStroke& key, uint32_t out[2]) {
  if (key.cp == 0) return 0;

  if (pendingMark != DeadMark::None) {
    const DeadMark mark = pendingMark;
    const uint32_t literal = pendingLiteral;
    deadKeyReset();

    if (key.cp == ' ') {          // "accent then space" = the accent character
      out[0] = literal;
      return 1;
    }
    const uint32_t composed = deadKeyCompose(mark, key.cp);
    if (composed != 0) {
      out[0] = composed;
      return 1;
    }
    // No accented form — including a second dead key. Print both, so nothing is
    // silently swallowed and no hidden state survives the keystroke.
    out[0] = literal;
    out[1] = key.cp;
    return 2;
  }

  if (key.mark != DeadMark::None) {
    pendingMark = key.mark;
    pendingLiteral = key.cp;
    return 0;
  }

  out[0] = key.cp;
  return 1;
}
