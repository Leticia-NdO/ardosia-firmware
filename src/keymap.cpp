#include "keymap.h"

namespace {

constexpr uint32_t CP_ACUTE      = 0x00B4;  // ´
constexpr uint32_t CP_GRAVE      = 0x0060;  // `
constexpr uint32_t CP_CIRCUMFLEX = 0x005E;  // ^
constexpr uint32_t CP_TILDE      = 0x007E;  // ~
constexpr uint32_t CP_DIAERESIS  = 0x00A8;  // ¨
constexpr uint32_t CP_CEDILLA_LO = 0x00E7;  // ç
constexpr uint32_t CP_CEDILLA_UP = 0x00C7;  // Ç

KeyStroke plain(uint32_t cp) { return KeyStroke{cp, DeadMark::None}; }
KeyStroke dead(uint32_t cp, DeadMark m) { return KeyStroke{cp, m}; }

// Letters and the number row sit at the same usages on both layouts.
bool isLetter(uint8_t hid) { return hid >= 0x04 && hid <= 0x1D; }
bool isNumberRow(uint8_t hid) { return hid >= 0x1E && hid <= 0x27; }

KeyStroke letter(uint8_t hid, bool upper) {
  return plain(static_cast<uint32_t>((upper ? 'A' : 'a') + (hid - 0x04)));
}

// ---------------------------------------------------------------------------
// US ANSI.  `intl` turns ' " ` ~ ^ into dead keys (US-International), which is
// the only way to reach á ã ç on a keyboard that has no accent keys at all.
// ---------------------------------------------------------------------------
KeyStroke usKey(uint8_t hid, bool shift, bool upper, bool intl) {
  if (isLetter(hid)) return letter(hid, upper);

  if (isNumberRow(hid)) {
    static const char un[] = "1234567890";
    static const char sh[] = "!@#$%^&*()";
    const int i = hid - 0x1E;
    if (!shift) return plain(static_cast<uint32_t>(un[i]));
    if (sh[i] == '^' && intl) return dead(CP_CIRCUMFLEX, DeadMark::Circumflex);
    return plain(static_cast<uint32_t>(sh[i]));
  }

  switch (hid) {
    case 0x28: return plain('\n');
    case 0x2B: return plain('\t');
    case 0x2C: return plain(' ');
    case 0x2D: return plain(shift ? '_' : '-');
    case 0x2E: return plain(shift ? '+' : '=');
    case 0x2F: return plain(shift ? '{' : '[');
    case 0x30: return plain(shift ? '}' : ']');
    case 0x31: return plain(shift ? '|' : '\\');
    case 0x33: return plain(shift ? ':' : ';');
    case 0x34:
      if (intl) return shift ? dead('"',  DeadMark::Diaeresis)
                             : dead('\'', DeadMark::Acute);
      return plain(shift ? '"' : '\'');
    case 0x35:
      if (intl) return shift ? dead(CP_TILDE, DeadMark::Tilde)
                             : dead(CP_GRAVE, DeadMark::Grave);
      return plain(shift ? '~' : '`');
    case 0x36: return plain(shift ? '<' : ',');
    case 0x37: return plain(shift ? '>' : '.');
    case 0x38: return plain(shift ? '?' : '/');
    default:   return plain(0);
  }
}

// ---------------------------------------------------------------------------
// ABNT2 (Brazilian).  An ISO-style board with one more key in the home row than
// US ANSI, so almost everything right of L is shifted one position across:
//
//   usage   US ANSI        ABNT2
//   0x2F    [ {            ´ `      (dead: acute / grave)
//   0x30    ] }            [ {
//   0x31    \ |            ] }
//   0x33    ; :            ç Ç      (a real key, not a composed character)
//   0x34    ' "            ~ ^      (dead: tilde / circumflex)
//   0x35    ` ~            ' "
//   0x38    / ?            ; :
//   0x64    -              \ |      (the key beside left Shift)
//   0x87    -              / ?      (International1, beside right Shift)
//   0x23+shift  ^          ¨        (dead: diaeresis, on the 6 key)
//
// This table is why "~" produced "ä" before: the ~ keycap sends 0x34, which the
// US map reads as the quote key, arming diaeresis instead of tilde.
// ---------------------------------------------------------------------------
KeyStroke abnt2Key(uint8_t hid, bool shift, bool upper) {
  if (isLetter(hid)) return letter(hid, upper);

  if (isNumberRow(hid)) {
    static const char un[] = "1234567890";
    static const char sh[] = "!@#$%?&*()";   // '?' is a placeholder for ¨ below
    const int i = hid - 0x1E;
    if (!shift) return plain(static_cast<uint32_t>(un[i]));
    if (i == 5) return dead(CP_DIAERESIS, DeadMark::Diaeresis);   // Shift+6 = ¨
    return plain(static_cast<uint32_t>(sh[i]));
  }

  switch (hid) {
    case 0x28: return plain('\n');
    case 0x2B: return plain('\t');
    case 0x2C: return plain(' ');
    case 0x2D: return plain(shift ? '_' : '-');
    case 0x2E: return plain(shift ? '+' : '=');
    case 0x2F: return shift ? dead(CP_GRAVE, DeadMark::Grave)
                            : dead(CP_ACUTE, DeadMark::Acute);
    case 0x30: return plain(shift ? '{' : '[');
    case 0x31: return plain(shift ? '}' : ']');
    case 0x33: return plain(upper ? CP_CEDILLA_UP : CP_CEDILLA_LO);
    case 0x34: return shift ? dead(CP_CIRCUMFLEX, DeadMark::Circumflex)
                            : dead(CP_TILDE, DeadMark::Tilde);
    case 0x35: return plain(shift ? '"' : '\'');
    case 0x36: return plain(shift ? '<' : ',');
    case 0x37: return plain(shift ? '>' : '.');
    case 0x38: return plain(shift ? ':' : ';');
    case 0x64: return plain(shift ? '|' : '\\');
    case 0x85: return plain(',');          // keypad comma (ABNT2 numpad)
    case 0x87: return plain(shift ? '?' : '/');
    default:   return plain(0);
  }
}

}  // namespace

KeyStroke keymapResolve(const KeyboardLayout layout, const uint8_t hid,
                        const uint8_t modifiers, const bool caps) {
  const bool shift = isShift(modifiers);
  const bool upper = shift ^ caps;

  switch (layout) {
    case KeyboardLayout::ABNT2:   return abnt2Key(hid, shift, upper);
    case KeyboardLayout::US_INTL: return usKey(hid, shift, upper, true);
    default:                      return usKey(hid, shift, upper, false);
  }
}
