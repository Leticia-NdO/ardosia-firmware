// Host-side tests for the pt-br input pipeline.
//
// The X4 has no serial console and every on-device test costs an SD card write
// plus a reboot, so the parts of this feature that are pure logic are built and
// exercised here with an ordinary compiler instead.  Covers: UTF-8 encoding and
// character-boundary walking, the US-International dead-key state machine, and
// the editor operations that had to stop counting bytes as characters.
//
// Run with test/run.sh — it is NOT part of the firmware build (src/CMakeLists
// globs src/ only) and does not need PlatformIO.

#include "../src/utf8_util.h"
#include "../src/keymap.h"
#include "../src/deadkeys.h"
#include "../src/text_editor.h"

#include <cstdio>
#include <initializer_list>
#include <utility>
#include <cstring>
#include <string>

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

// Encode a codepoint into a std::string, for readable comparisons.
static std::string enc(uint32_t cp) {
  char b[5];
  const int n = utf8Encode(cp, b);
  return std::string(b, n > 0 ? n : 0);
}

// ---------------------------------------------------------------------------

static void testEncoding() {
  printf("utf8Encode / boundaries\n");

  check(enc(0x61)  == "a",        "ascii 'a' is one byte");
  check(enc(0x00E1) == "\xC3\xA1", "á encodes as C3 A1");
  check(enc(0x00E7) == "\xC3\xA7", "ç encodes as C3 A7");
  check(enc(0x0178) == "\xC5\xB8", "Ÿ encodes as C5 B8");
  char sbuf[4];
  check(utf8Encode(0xD800, sbuf) == 0, "surrogates are rejected");

  // "aáb" = 61 C3 A1 62
  const char* s = "a\xC3\xA1" "b";
  check(utf8SeqLen((unsigned char)s[0]) == 1, "lead byte of 'a' is 1 byte");
  check(utf8SeqLen((unsigned char)s[1]) == 2, "lead byte of 'á' is 2 bytes");
  check(utf8IsLead(s[1]) && !utf8IsLead(s[2]), "continuation byte is not a lead");

  check(utf8NextStart(s, 4, 0) == 1, "next after 'a' is 1");
  check(utf8NextStart(s, 4, 1) == 3, "next after 'á' skips both bytes");
  check(utf8PrevStart(s, 3) == 1,    "prev from 3 lands on the start of 'á'");
  check(utf8PrevStart(s, 4) == 3,    "prev from end lands on 'b'");
  check(utf8PrevStart(s, 0) == 0,    "prev at 0 stays at 0");
  check(utf8CountRange(s, 0, 4) == 3, "\"aáb\" is 3 characters in 4 bytes");
  check(utf8AdvanceCodepoints(s, 0, 4, 2) == 3, "2 characters in = byte 3");
  check(utf8AdvanceCodepoints(s, 0, 4, 99) == 4, "advancing past the end clamps");
}

// ---------------------------------------------------------------------------

// Shorthand: resolve a key on a layout, then push it through the composer.
// out is appended to `sink` as UTF-8, so a test reads like what you would type.
static std::string typeKeys(KeyboardLayout layout,
                            std::initializer_list<std::pair<uint8_t,uint8_t>> keys) {
  deadKeyReset();
  std::string r;
  for (auto& k : keys) {
    uint32_t out[2];
    const int n = deadKeyFeed(keymapResolve(layout, k.first, k.second, false), out);
    for (int i = 0; i < n; i++) r += enc(out[i]);
  }
  return r;
}

// HID usage codes, by physical position.
static const uint8_t K_6 = 0x23, K_A = 0x04, K_C = 0x06, K_E = 0x08;
static const uint8_t K_N = 0x11, K_O = 0x12, K_U = 0x18, K_T = 0x17;
static const uint8_t K_G = 0x0A, K_V = 0x19;
static const uint8_t K_SPACE = 0x2C;
static const uint8_t K_LBRACKET = 0x2F;   // ABNT2: ´ `
static const uint8_t K_SEMI     = 0x33;   // ABNT2: ç Ç
static const uint8_t K_QUOTE    = 0x34;   // ABNT2: ~ ^     <- the key that broke
static const uint8_t K_GRAVE    = 0x35;   // ABNT2: ' "
static const uint8_t K_SLASH    = 0x38;   // ABNT2: ; :
static const uint8_t K_INTL1    = 0x87;   // ABNT2: / ?
static const uint8_t K_NONUS    = 0x64;   // ABNT2: \\ |
static const uint8_t SH = MOD_SHIFT_LEFT, NO = 0;

static void testKeymapAbnt2() {
  printf("ABNT2 layout\n");
  const auto L = KeyboardLayout::ABNT2;

  // The regression that started this: the ~ keycap sends usage 0x34, which the
  // US map reads as the quote key — so "~" then "a" used to produce "ä".
  checkStr(typeKeys(L, {{K_QUOTE,NO},{K_A,NO}}).c_str(), "ã", "~a gives ã (was ä under the US map)");
  checkStr(typeKeys(L, {{K_QUOTE,NO},{K_O,NO}}).c_str(), "õ", "~o gives õ");
  checkStr(typeKeys(L, {{K_QUOTE,NO},{K_N,NO}}).c_str(), "ñ", "~n gives ñ");
  checkStr(typeKeys(L, {{K_QUOTE,SH},{K_E,NO}}).c_str(), "ê", "Shift+that key is ^, so ^e gives ê");
  checkStr(typeKeys(L, {{K_QUOTE,SH},{K_A,NO}}).c_str(), "â", "^a gives â");

  // acute / grave live on the key US ANSI uses for [
  checkStr(typeKeys(L, {{K_LBRACKET,NO},{K_A,NO}}).c_str(), "á", "´a gives á");
  checkStr(typeKeys(L, {{K_LBRACKET,NO},{K_E,NO}}).c_str(), "é", "´e gives é");
  checkStr(typeKeys(L, {{K_LBRACKET,SH},{K_A,NO}}).c_str(), "à", "`a gives à");
  checkStr(typeKeys(L, {{K_LBRACKET,NO},{K_C,NO}}).c_str(), "ç", "´c gives ç");

  // ¨ is Shift+6 on ABNT2, where US ANSI has ^
  checkStr(typeKeys(L, {{K_6,SH},{K_U,NO}}).c_str(), "ü", "Shift+6 is ¨, so ¨u gives ü");
  checkStr(typeKeys(L, {{K_6,NO}}).c_str(), "6", "6 unshifted is still 6");

  // ç is a real key, not a composed character
  checkStr(typeKeys(L, {{K_SEMI,NO}}).c_str(), "ç", "the Ç key types ç directly");
  checkStr(typeKeys(L, {{K_SEMI,SH}}).c_str(), "Ç", "shifted gives Ç");

  // the keys that moved
  checkStr(typeKeys(L, {{K_GRAVE,NO}}).c_str(), "'",  "the key left of 1 is the apostrophe");
  checkStr(typeKeys(L, {{K_GRAVE,SH}}).c_str(), "\"", "shifted it is the quote");
  checkStr(typeKeys(L, {{K_SLASH,NO}}).c_str(), ";",  "US ANSI's / key is ; on ABNT2");
  checkStr(typeKeys(L, {{K_SLASH,SH}}).c_str(), ":",  "shifted it is :");
  checkStr(typeKeys(L, {{K_INTL1,NO}}).c_str(), "/",  "International1 is the slash");
  checkStr(typeKeys(L, {{K_INTL1,SH}}).c_str(), "?",  "shifted it is the question mark");
  checkStr(typeKeys(L, {{K_NONUS,NO}}).c_str(), "\\", "the key beside left Shift is backslash");

  // literal accents, and the no-compose fallback
  checkStr(typeKeys(L, {{K_QUOTE,NO},{K_SPACE,NO}}).c_str(), "~", "~ then space gives a literal ~");
  checkStr(typeKeys(L, {{K_LBRACKET,NO},{K_SPACE,NO}}).c_str(), "´", "´ then space gives a literal ´");
  checkStr(typeKeys(L, {{K_QUOTE,NO},{K_T,NO}}).c_str(), "~t", "~t has no composed form: both print");
  checkStr(typeKeys(L, {{K_QUOTE,NO},{K_QUOTE,NO}}).c_str(), "~~", "two tildes print as two tildes");
  check(deadKeyPending() == 0, "nothing stays armed");

  // uppercase, including the derived-capital path
  checkStr(typeKeys(L, {{K_LBRACKET,NO},{K_A,SH}}).c_str(), "Á", "´A gives Á");
  checkStr(typeKeys(L, {{K_QUOTE,NO},{K_A,SH}}).c_str(),    "Ã", "~A gives Ã");
  checkStr(typeKeys(L, {{K_LBRACKET,NO},{K_C,SH}}).c_str(), "Ç", "´C gives Ç");

  // whole words, keyed exactly the way they are actually typed
  checkStr(typeKeys(L, {{K_SEMI,NO},{K_QUOTE,NO},{K_A,NO},{K_O,NO}}).c_str(),
           "ção", "ç ~ a o types \"ção\"");
  checkStr(typeKeys(L, {{K_LBRACKET,NO},{K_A,NO},{K_G,NO},{K_U,NO},{K_A,NO}}).c_str(),
           "água", "´ a g u a types \"água\"");
  checkStr(typeKeys(L, {{K_V,NO},{K_O,NO},{K_C,NO},{K_QUOTE,SH},{K_E,NO}}).c_str(),
           "você", "v o c ^ e types \"você\"");
}

static void testKeymapUs() {
  printf("US / US-International layouts\n");

  // Plain US: the marks are literal characters, nothing composes.
  const auto U = KeyboardLayout::US;
  checkStr(typeKeys(U, {{K_QUOTE,NO}}).c_str(), "'",  "US: 0x34 is the apostrophe");
  checkStr(typeKeys(U, {{K_GRAVE,SH}}).c_str(), "~",  "US: Shift+backquote is a literal ~");
  checkStr(typeKeys(U, {{K_6,SH}}).c_str(),     "^",  "US: Shift+6 is a literal ^");
  checkStr(typeKeys(U, {{K_QUOTE,NO},{K_A,NO}}).c_str(), "'a", "US: no composition");
  check(deadKeyPending() == 0, "US arms nothing");

  // US-International: same keys, now dead.
  const auto I = KeyboardLayout::US_INTL;
  checkStr(typeKeys(I, {{K_QUOTE,NO},{K_A,NO}}).c_str(), "á",  "US-Intl: 'a gives á");
  checkStr(typeKeys(I, {{K_QUOTE,NO},{K_C,NO}}).c_str(), "ç",  "US-Intl: 'c gives ç");
  checkStr(typeKeys(I, {{K_GRAVE,SH},{K_A,NO}}).c_str(), "ã",  "US-Intl: ~a gives ã");
  checkStr(typeKeys(I, {{K_6,SH},{K_E,NO}}).c_str(),     "ê",  "US-Intl: ^e gives ê");
  checkStr(typeKeys(I, {{K_QUOTE,SH},{K_U,NO}}).c_str(), "ü",  "US-Intl: \"u gives ü");
  checkStr(typeKeys(I, {{K_QUOTE,NO},{K_SPACE,NO}}).c_str(), "'", "US-Intl: ' then space");
  checkStr(typeKeys(I, {{K_QUOTE,NO},{K_T,NO}}).c_str(), "'t", "US-Intl: 't prints both");

  // The bug, stated as a test: reading an ABNT2 board with the US-Intl map.
  checkStr(typeKeys(I, {{K_QUOTE,SH},{K_A,NO}}).c_str(), "ä",
           "US-Intl on 0x34+shift gives ä - this is what the ABNT2 ~ key hit");
}

static void testCompose() {
  printf("composition table\n");
  check(deadKeyCompose(DeadMark::Acute, 'a') == 0x00E1, "acute + a");
  check(deadKeyCompose(DeadMark::Acute, 'A') == 0x00C1, "acute + A (derived capital)");
  check(deadKeyCompose(DeadMark::Diaeresis, 'Y') == 0x0178, "diaeresis + Y is U+0178, not 0xDF");
  check(deadKeyCompose(DeadMark::Tilde, 'e') == 0, "tilde + e has no composed form");
  check(deadKeyCompose(DeadMark::None, 'a') == 0, "no mark composes nothing");
  check(deadKeyCompose(DeadMark::Acute, 0x00E1) == 0, "a non-ASCII base composes nothing");

  // reset disarms (what Esc and the arrow keys do)
  uint32_t out[2];
  deadKeyFeed(keymapResolve(KeyboardLayout::ABNT2, K_QUOTE, NO, false), out);
  check(deadKeyPending() == 0x007E, "tilde armed, and it reports its own character");
  deadKeyReset();
  check(deadKeyPending() == 0, "reset disarms");
}

// ---------------------------------------------------------------------------

static void type(const char* utf8) {
  const unsigned char* p = (const unsigned char*)utf8;
  while (*p) {
    uint32_t cp = 0;
    const int len = utf8SeqLen(*p);
    if (len == 1) cp = *p;
    else {
      cp = *p & ((1 << (7 - len)) - 1);
      for (int i = 1; i < len; i++) cp = (cp << 6) | (p[i] & 0x3F);
    }
    editorInsertCodepoint(cp);
    p += len;
  }
}

static void testEditor() {
  printf("editor: characters, not bytes\n");

  editorInit();
  editorSetCharsPerLine(80);
  type("ação");
  checkStr(editorGetBuffer(), "ação", "typing \"ação\" round-trips");
  check(editorGetLength() == 6, "\"ação\" is 6 bytes");
  check(editorGetCursorPosition() == 6, "cursor sits past the last byte");

  // backspace removes a whole character, not half of one
  editorDeleteChar();
  checkStr(editorGetBuffer(), "açã", "backspace removes the 1-byte 'o'");
  check(editorGetLength() == 5, "\"açã\" is 5 bytes");
  editorDeleteChar();
  checkStr(editorGetBuffer(), "aç", "backspace removes both bytes of 'ã'");
  check(editorGetLength() == 3, "\"aç\" is 3 bytes");

  // left/right step whole characters
  editorInit();
  editorSetCharsPerLine(80);
  type("aáb");
  check(editorGetCursorPosition() == 4, "cursor at end of \"aáb\" (4 bytes)");
  editorMoveCursorLeft();
  check(editorGetCursorPosition() == 3, "left skips 'b'");
  editorMoveCursorLeft();
  check(editorGetCursorPosition() == 1, "left skips both bytes of 'á'");
  editorMoveCursorRight();
  check(editorGetCursorPosition() == 3, "right skips both bytes of 'á'");

  // delete forward removes a whole character
  editorInit();
  editorSetCharsPerLine(80);
  type("áb");
  editorMoveCursorHome();
  check(editorGetCursorPosition() == 0, "home");
  editorDeleteForward();
  checkStr(editorGetBuffer(), "b", "delete removes the whole 'á'");

  // Word wrap counts characters, not bytes.  4 accented characters are 8 bytes:
  // they fit a 5-column line, but byte counting would have wrapped at the 3rd.
  editorInit();
  editorSetCharsPerLine(5);
  type("áááá");
  check(editorGetLineCount() == 1,
        "4 accented characters (8 bytes) still fit a 5-column line");
  type("áá");                          // now past the column limit
  check(editorGetLineCount() == 2, "crossing 5 columns wraps");
  check(utf8IsLead(editorGetBuffer()[editorGetLinePosition(1)]),
        "the wrap point lands on a character boundary");

  // up/down preserve the visual column across an accented line
  editorInit();
  editorSetCharsPerLine(80);
  type("ááá\nabcd");                   // line 0: 3 chars / 6 bytes, line 1: 4 chars
  editorMoveCursorHome();              // start of "abcd"
  editorMoveCursorRight();
  editorMoveCursorRight();             // visual column 2 on line 1
  check(editorGetCursorPosition() == 9, "column 2 of line 1 is byte 9");
  editorMoveCursorUp();
  check(editorGetCursorPosition() == 4,
        "column 2 of the accented line is byte 4, not byte 2");
  editorMoveCursorDown();
  check(editorGetCursorPosition() == 9, "and back down to the same column");

  // The same test in the other direction. Moving up FROM an ASCII line is
  // insensitive by construction — there the byte offset and the visual column
  // are the same number — so leaving an ACCENTED line is what actually catches
  // a byte-based column. (Found by mutation testing: the check above passed
  // even with the fix reverted.)
  editorInit();
  editorSetCharsPerLine(80);
  type("abcd\nááá");                  // line 0: 4 ascii, line 1: 3 accented
  editorMoveCursorHome();              // start of the accented line, byte 5
  editorMoveCursorRight();
  editorMoveCursorRight();             // visual column 2
  check(editorGetCursorPosition() == 9, "column 2 of the accented line is byte 9");
  editorMoveCursorUp();
  check(editorGetCursorPosition() == 2,
        "up to the ascii line keeps visual column 2, not byte offset 4");

  // word count is unaffected by multi-byte characters
  editorInit();
  editorSetCharsPerLine(80);
  type("ação é boa");
  check(editorGetWordCount() == 3, "\"ação é boa\" is 3 words");
}

int main() {
  testEncoding();
  testCompose();
  testKeymapAbnt2();
  testKeymapUs();
  testEditor();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
