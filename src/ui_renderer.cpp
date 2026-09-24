#include "ui_renderer.h"
#include "config.h"
#include "ui_layout.h"
#include "dpad.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "input_handler.h"
#include "confirm.h"
#include "quickmenu.h"
#include "utf8_util.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalDisplay.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <cstring>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern ConfirmDialog confirmDialog;
extern QuickMenu quickMenu;
extern WritingMode writingMode;
extern FontSize fontSize;
extern EditorFont editorFont;
extern DpadMode dpadMode;
extern KeyboardLayout keyboardLayout;
extern SleepScreenMode sleepScreenMode;
extern bool showWordCount;

// External functions
uint32_t getCurrentPasskey();
bool isDeviceScanning();
uint32_t getScanAgeMs();

// Font data includes
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/jetbrainsmono_9_regular.h>
#include <builtinFonts/jetbrainsmono_9_bold.h>
#include <builtinFonts/jetbrainsmono_11_regular.h>
#include <builtinFonts/jetbrainsmono_11_bold.h>
#include <builtinFonts/jetbrainsmono_13_regular.h>
#include <builtinFonts/jetbrainsmono_13_bold.h>
#include <builtinFonts/jetbrainsmono_18_regular.h>
#include <builtinFonts/jetbrainsmono_18_bold.h>
#include <builtinFonts/jetbrainsmono_12_regular.h>
#include <builtinFonts/jetbrainsmono_14_regular.h>
#include <builtinFonts/jetbrainsmono_16_regular.h>

// Font objects (file-scoped)
static EpdFont ns16Regular(&notosans_16_regular);
static EpdFont ns16Bold(&notosans_16_bold);
static EpdFontFamily ns16Family(&ns16Regular, &ns16Bold);

static EpdFont ns14Regular(&notosans_14_regular);
static EpdFont ns14Bold(&notosans_14_bold);
static EpdFontFamily ns14Family(&ns14Regular, &ns14Bold);

static EpdFont ns12Regular(&notosans_12_regular);
static EpdFont ns12Bold(&notosans_12_bold);
static EpdFontFamily ns12Family(&ns12Regular, &ns12Bold);

// Chrome faces. Sizes picked by measuring ink height, not by the nominal
// number: jetbrainsmono_13 inks 31px against notosans_12's 30, and
// jetbrainsmono_15 inks 35px against notosans_14's 35. So the bands and row
// pitches the layout helpers derive stay where they were and only the
// typeface changes. (Monospace is 30-48% wider for the same ink height —
// that part does move, and truncation absorbs it.)
static EpdFont jbm9Regular(&jetbrainsmono_9_regular);
static EpdFont jbm9Bold(&jetbrainsmono_9_bold);
static EpdFontFamily jbm9Family(&jbm9Regular, &jbm9Bold);

static EpdFont jbm11Regular(&jetbrainsmono_11_regular);
static EpdFont jbm11Bold(&jetbrainsmono_11_bold);
static EpdFontFamily jbm11Family(&jbm11Regular, &jbm11Bold);

static EpdFont jbm13Regular(&jetbrainsmono_13_regular);
static EpdFont jbm13Bold(&jetbrainsmono_13_bold);
static EpdFontFamily jbm13Family(&jbm13Regular, &jbm13Bold);

static EpdFont jbm18Regular(&jetbrainsmono_18_regular);
static EpdFont jbm18Bold(&jetbrainsmono_18_bold);
static EpdFontFamily jbm18Family(&jbm18Regular, &jbm18Bold);

// Editor faces for Editor Font: Mono. No bold slot — see FONT_EDIT_MONO_* in
// config.h for why that is safe.
static EpdFont jbm12Regular(&jetbrainsmono_12_regular);
static EpdFontFamily jbm12Family(&jbm12Regular);
static EpdFont jbm14Regular(&jetbrainsmono_14_regular);
static EpdFontFamily jbm14Family(&jbm14Regular);
static EpdFont jbm16Regular(&jetbrainsmono_16_regular);
static EpdFontFamily jbm16Family(&jbm16Regular);

// OTA app detection (defined in main.cpp)
extern OtaAppEntry otaApps[];
extern int otaAppCount;

// Extern shared state (defined in main.cpp)
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int settingsTab;
extern NoteSort noteSort;
extern int bluetoothDeviceSelection;
extern int pairedKeyboardSelection;
extern Orientation currentOrientation;
extern int charsPerLine;
extern char renameBuffer[];
extern int renameBufferLen;

void rendererSetup(GfxRenderer& renderer) {
  // Editor faces — reached through editorFontId(), never drawn as chrome.
  // ubuntu_10 used to be registered here as FONT_SMALL; the chrome swap left
  // it undrawn anywhere, so it came out of the build with its 26 KB.
  renderer.insertFont(FONT_LARGE, ns16Family);
  renderer.insertFont(FONT_BODY, ns14Family);
  renderer.insertFont(FONT_UI, ns12Family);
  // Chrome faces.
  renderer.insertFont(FONT_CHROME_S, jbm9Family);
  renderer.insertFont(FONT_CHROME_M, jbm11Family);
  renderer.insertFont(FONT_CHROME_L, jbm13Family);
  renderer.insertFont(FONT_CHROME_XL, jbm18Family);
  renderer.insertFont(FONT_EDIT_MONO_S, jbm12Family);
  renderer.insertFont(FONT_EDIT_MONO_M, jbm14Family);
  renderer.insertFont(FONT_EDIT_MONO_L, jbm16Family);
}

// ---------------------------------------------------------------------------
// Vertical metrics
//
// The arithmetic lives in ui_layout.cpp, free of renderer and font blob so the
// host suite can exercise it; the reason it has to exist at all is documented
// there. What stays here is the half that needs the fonts: turning a font id
// into the glyph measurements ui_layout works on.
// ---------------------------------------------------------------------------

static const EpdFont& fontFor(int fontId) {
  if (fontId == FONT_LARGE)    return ns16Regular;
  if (fontId == FONT_BODY)     return ns14Regular;
  if (fontId == FONT_UI)       return ns12Regular;
  if (fontId == FONT_EDIT_MONO_S)  return jbm12Regular;
  if (fontId == FONT_EDIT_MONO_M)  return jbm14Regular;
  if (fontId == FONT_EDIT_MONO_L)  return jbm16Regular;
  if (fontId == FONT_CHROME_XL) return jbm18Regular;
  if (fontId == FONT_CHROME_L)  return jbm13Regular;
  if (fontId == FONT_CHROME_M)  return jbm11Regular;
  return jbm9Regular;   // FONT_CHROME_S, and the fallback: chrome is monospace
}

// Look the glyphs up and let ui_layout decide what they mean. 'Á' is the
// reference for the top of the ink, with 'A' as the fallback for a font that
// has no accents.
static TextInk inkOf(int fontId) {
  const EpdFont& f = fontFor(fontId);
  const EpdGlyph* tall = f.getGlyph(0x00C1);   // 'Á'
  if (!tall) tall = f.getGlyph('A');
  const EpdGlyph* low = f.getGlyph('g');
  return inkFromMetrics(f.data->ascender, f.data->descender,
                        tall != nullptr, tall ? tall->top : 0,
                        low != nullptr, low ? low->height : 0, low ? low->top : 0);
}

// Thin wrappers so call sites keep naming a font instead of an ink box.
static int bandHeight(int fontId) { return bandHeightOf(inkOf(fontId)); }
static int rowPitch(int fontId)   { return rowPitchOf(inkOf(fontId)); }
static int lineStep(int fontId)   { return lineStepOf(inkOf(fontId)); }

static int textYInBand(int fontId, int bandTop, int bandH) {
  return textYInBandOf(inkOf(fontId), bandTop, bandH);
}

// ---------------------------------------------------------------------------
// Clipped draw helpers — use renderer.truncatedText() so NO pixel ever
// exceeds screen width.  This is how crosspoint-reader prevents GFX errors.
// ---------------------------------------------------------------------------

// Draw text that is guaranteed not to overflow the screen width.
// maxW = available pixel width from x to right edge (caller computes).
// Falls back to sw - x - 5 if maxW <= 0.
static void drawClippedText(GfxRenderer& r, int font, int x, int y,
                            const char* text, int maxW = 0,
                            bool black = true,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0 || x >= sw || y < 0 || y >= sh) return;

  if (maxW <= 0) maxW = sw - x - 5;   // 5px right margin
  if (maxW <= 0) return;

  auto clipped = r.truncatedText(font, text, maxW, style);
  if (!clipped.empty()) {
    r.drawText(font, x, y, clipped.c_str(), black, style);
  }
}

// Draw right-aligned text (e.g. battery %, RSSI, settings values).
// Computes its own X from the measured text width.
static void drawRightText(GfxRenderer& r, int font, int rightEdge, int y,
                          const char* text, bool black = true,
                          EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  // Use getTextWidth (bounding box) — same measurement truncatedText uses —
  // so the allocated space always matches what the truncation check expects.
  int tw = r.getTextWidth(font, text, style);
  if (tw <= 0) tw = 30;                    // safe fallback
  int x = rightEdge - tw;
  if (x < 5) x = 5;                        // don't go off left edge
  drawClippedText(r, font, x, y, text, rightEdge - x, black, style);
}


// Safe fillRect — clamp dimensions to screen
static void clippedFillRect(GfxRenderer& r, int x, int y, int w, int h,
                            bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x;
  if (y + h > sh) h = sh - y;
  if (w > 0 && h > 0) r.fillRect(x, y, w, h, state);
}

// ---------------------------------------------------------------------------
// Header and footer content
// ---------------------------------------------------------------------------

// "76% [||||_]" — the design draws the charge as a five-cell meter next to the
// number. Rounded to the nearest cell rather than truncated, so a full battery
// is the only thing that shows five bars and an empty one the only thing that
// shows none.
static void batteryText(HalGPIO& gpio, char* out, size_t n) {
  int pct = gpio.getBatteryPercentage();
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  int cells = (pct + 10) / 20;
  if (cells > 5) cells = 5;
  char meter[6];
  for (int i = 0; i < 5; i++) meter[i] = (i < cells) ? '|' : '_';
  meter[5] = '\0';
  snprintf(out, n, "%d%% [%s]", pct, meter);
}

// "[ KB Connected ]" — the bracketed form the design uses in the footer's right
// column. Kept short: this column never wraps, so it has to stay narrow enough
// to leave the middle one room at 440px.
static const char* bleStatusText() {
  switch (getConnectionState()) {
    case BLEState::CONNECTED:  return "[ KB Connected ]";
    case BLEState::SCANNING:   return "[ Scanning ]";
    case BLEState::CONNECTING: return "[ Connecting ]";
    default:                   return "[ No KB ]";
  }
}

// ---------------------------------------------------------------------------
// The shared screen frame
// ---------------------------------------------------------------------------

// What the footer carries. The middle column has two forms because it is the
// only one that reflows: `centreWide` on one line when the three columns fit,
// `centreA`/`centreB` stacked when they do not. Leave the middle null for a
// screen that only has the outer two.
struct FooterText {
  const char* left = nullptr;
  const char* centreWide = nullptr;
  const char* centreA = nullptr;
  const char* centreB = nullptr;
  const char* right = nullptr;   // null -> the BLE status
};

// The usable box a screen may draw its body into.
struct ScreenFrame {
  int x, w;         // content column
  int top, bottom;  // usable rows, [top, bottom)
};

static int textW(GfxRenderer& r, int font, const char* s,
                 EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  return (s && s[0]) ? r.getTextWidth(font, s, style) : 0;
}

// Clears the screen, draws the border, the header band and the footer band, and
// hands back what is left. Every menu-family screen starts with this; the text
// editor does not, because its header carries a different set of things and the
// design does not cover it yet.
static ScreenFrame drawScreenFrame(GfxRenderer& renderer, HalGPIO& gpio,
                                   const char* title, const FooterText& ft) {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;

  renderer.clearScreen();
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Outer border. Note the width/height convention: the lineWidth overload of
  // drawRect treats x + width as the LAST column it paints (the three-argument
  // one uses x + width - 1), so a full-screen border wants sw - 1, not sw.
  // Passing sw would send four pixels per redraw past the panel edge, where
  // drawPixel drops them but logs each one to Serial first.
  renderer.drawRect(FRAME_INSET, FRAME_INSET,
                    sw - 1 - 2 * FRAME_INSET, sh - 1 - 2 * FRAME_INSET,
                    FRAME_BORDER, tc);

  // --- header ---
  const HeaderLayout head = headerLayoutOf(inkOf(FONT_CHROME_XL));
  const int padX = FRAME_INSET + FRAME_BORDER + HEADER_PAD_X;

  char batt[24];
  batteryText(gpio, batt, sizeof(batt));
  const int battW = textW(renderer, FONT_CHROME_S, batt, EpdFontFamily::BOLD);

  drawClippedText(renderer, FONT_CHROME_XL, padX,
                  textYInBandOf(inkOf(FONT_CHROME_XL), head.boxTop, head.boxH),
                  title, sw - 2 * padX - battW - 16, tc, EpdFontFamily::BOLD);
  drawRightText(renderer, FONT_CHROME_S, sw - padX,
                textYInBandOf(inkOf(FONT_CHROME_S), head.boxTop, head.boxH),
                batt, tc, EpdFontFamily::BOLD);
  clippedFillRect(renderer, FRAME_INSET + FRAME_BORDER, head.ruleY,
                  sw - 2 * (FRAME_INSET + FRAME_BORDER), HEADER_RULE, tc);

  // --- footer ---
  const TextInk fInk = inkOf(FONT_CHROME_S);
  const char* right = ft.right ? ft.right : bleStatusText();
  const int contentW = contentWOf(sw);
  const FooterLayout foot =
      footerLayoutOf(sh, fInk, contentW,
                     textW(renderer, FONT_CHROME_S, ft.left),
                     textW(renderer, FONT_CHROME_S, ft.centreWide),
                     textW(renderer, FONT_CHROME_S, right, EpdFontFamily::BOLD),
                     ft.centreA != nullptr && ft.centreB != nullptr);

  clippedFillRect(renderer, FRAME_INSET + FRAME_BORDER, foot.ruleY,
                  sw - 2 * (FRAME_INSET + FRAME_BORDER), FOOTER_RULE, tc);

  const int fPadX = FRAME_INSET + FRAME_BORDER + FOOTER_PAD_X;
  const int textY = textYInBandOf(fInk, foot.boxTop, foot.boxH);

  // The middle column is placed first, because it is centred on the screen and
  // the outer two have to be told where it starts and ends.
  //
  // Each column is then clipped to its OWN share of the width. Without that the
  // left one is drawn with no bound and simply runs underneath the middle —
  // invisible while every string was short, obvious the moment a long one
  // (a diagnostic readout, a wordy hint) landed there.
  int centreX = 0, centreW = 0;
  if (foot.rows == 1 && ft.centreWide) {
    centreW = textW(renderer, FONT_CHROME_S, ft.centreWide);
  } else if (foot.rows == 2) {
    const int a = textW(renderer, FONT_CHROME_S, ft.centreA);
    const int b = textW(renderer, FONT_CHROME_S, ft.centreB);
    centreW = a > b ? a : b;
  }
  if (centreW > 0) {
    // Never let the middle take so much that the sides have nothing left.
    const int centreMax = sw - 2 * fPadX - 2 * FOOTER_COL_GAP;
    if (centreW > centreMax) centreW = centreMax;
    centreX = (sw - centreW) / 2;
  }

  const int rightStart = centreW > 0 ? centreX + centreW + FOOTER_COL_GAP : fPadX;

  // The right column is right-anchored; work out where it actually begins so
  // the left one can stop before it. With no middle column the left would
  // otherwise run all the way to the edge and under it.
  int rightX = sw - fPadX;
  if (right && right[0]) {
    const int rw = textW(renderer, FONT_CHROME_S, right, EpdFontFamily::BOLD);
    rightX = sw - fPadX - rw;
    if (rightX < rightStart) rightX = rightStart;
    drawClippedText(renderer, FONT_CHROME_S, rightX, textY, right,
                    sw - fPadX - rightX, tc, EpdFontFamily::BOLD);
  }

  int leftLimit = rightX;
  if (centreW > 0 && centreX < leftLimit) leftLimit = centreX;
  leftLimit -= FOOTER_COL_GAP;

  if (ft.left && leftLimit > fPadX) {
    drawClippedText(renderer, FONT_CHROME_S, fPadX, textY, ft.left,
                    leftLimit - fPadX, tc);
  }

  if (centreW > 0) {
    if (foot.rows == 1) {
      drawClippedText(renderer, FONT_CHROME_S, centreX, textY, ft.centreWide, centreW, tc);
    } else {
      const char* rows[2] = {ft.centreA, ft.centreB};
      for (int i = 0; i < 2; i++) {
        if (!rows[i]) continue;
        const int w = textW(renderer, FONT_CHROME_S, rows[i]);
        const int rowTop = foot.boxTop + i * lineStepOf(fInk);
        drawClippedText(renderer, FONT_CHROME_S, (sw - w) / 2,
                        textYInBandOf(fInk, rowTop, fInk.height()), rows[i], w + 4, tc);
      }
    }
  }

  ScreenFrame box;
  box.x = contentXOf();
  box.w = contentW;
  box.top = bodyTopOf(head) + WORKSPACE_PAD;
  box.bottom = bodyBottomOf(foot) - WORKSPACE_PAD;
  return box;
}

// A row's keyboard shortcut, in a column of its own: "[3]  Settings", the way
// the design draws it. The digit IS the row index, computed where the row is
// drawn, so it cannot drift out of step with the label beside it.
//
// Past MENU_MAX_NUMBERED no digit reaches the row, so the column is left blank
// rather than printing a "[10]" that no key produces — but the label still
// starts at the same x, so the list does not go ragged.
static constexpr int DIGIT_GAP = 12;   // between the digit column and the label

// Monospace, so every digit measures the same; "[9]" is just a concrete sample.
// Measured BOLD because that is the wider of the two weights the column is
// drawn in, so the label's x never shifts as the selection moves.
static int digitColumnW(GfxRenderer& r) {
  return r.getTextWidth(FONT_CHROME_M, "[9]", EpdFontFamily::BOLD);
}

static void drawRowDigit(GfxRenderer& r, int x, int bandTop, int bandH, int index,
                         bool selected) {
  if (index >= MENU_MAX_NUMBERED) return;
  // The tenth row is reached by pressing 0, so that is what it shows. Printing
  // "[10]" would name a key that does not exist and would be wider than the
  // column every other row measured itself against.
  char buf[8];
  snprintf(buf, sizeof(buf), "[%d]", (index + 1 == 10) ? 0 : index + 1);
  const bool tc = !darkMode;
  // The design greys this column so it recedes behind the label. With no grey
  // available the same job falls to weight, which means the digit has to be the
  // LIGHTER of the two — drawing it bold against a regular label would invert
  // the hierarchy the grey was there to create. It follows the row into bold
  // only when selected, where the design has both in 700 on the inverted band.
  drawClippedText(r, FONT_CHROME_M, x,
                  textYInBandOf(inkOf(FONT_CHROME_M), bandTop, bandH), buf, 0,
                  selected ? !tc : tc,
                  selected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
}

// The chevron the design puts on the selected row, meaning "Enter opens this".
// U+203A is a real glyph in the converted font — fontconvert's charset covers
// General Punctuation — so it needs no bitmap of its own. Written as explicit
// UTF-8 rather than \u203a so it cannot depend on the compiler's execution
// charset; everything else in this firmware treats text as UTF-8 bytes too.
static const char CHEVRON[] = "\xE2\x80\xBA";   // U+203A, single right angle quote

static void drawRowChevron(GfxRenderer& r, int rightEdge, int bandTop, int bandH,
                           bool selected) {
  const bool tc = !darkMode;
  drawRightText(r, FONT_CHROME_L, rightEdge,
                textYInBandOf(inkOf(FONT_CHROME_L), bandTop, bandH), CHEVRON,
                selected ? !tc : tc, EpdFontFamily::BOLD);
}

// Draws the scrollbar for a list, and reports how much width the rows have to
// give up for it. Nothing is drawn — and nothing is taken — when the list fits.
static int drawListScrollbar(GfxRenderer& r, const ScreenFrame& box, int listTop,
                             int total, int visible, int start) {
  const int trackH = box.top + (box.bottom - box.top) - listTop;
  const ScrollThumb th = scrollThumbOf(trackH, total, visible, start);
  if (th.h <= 0) return 0;

  const bool tc = !darkMode;
  const int x = box.x + box.w - SCROLLBAR_W;
  // Track as a hairline, thumb as a solid bar: on e-ink a filled track would
  // read as a second selection band.
  clippedFillRect(r, x + SCROLLBAR_W / 2, listTop, 1, trackH, tc);
  clippedFillRect(r, x, listTop + th.y, SCROLLBAR_W, th.h, tc);
  return SCROLLBAR_W + SCROLLBAR_GAP;
}

// "1,240 words" for the notes list, or an em dash when the count has never been
// taken — a note that predates the sidecar's word column, or that arrived over
// sync. A dash says "not known"; a 0 would claim the note is empty.
static void wordCountText(uint32_t words, char* out, size_t n) {
  if (words == NOTE_WORDS_UNKNOWN) { snprintf(out, n, "\xE2\x80\x94"); return; }  // U+2014
  if (words == 1) { snprintf(out, n, "1 word"); return; }
  if (words < 1000) { snprintf(out, n, "%lu words", (unsigned long)words); return; }
  // Thousands separator. English UI, so a comma — the prototype's "1.240 pal."
  // is the pt-br form, and translating the interface is its own project.
  snprintf(out, n, "%lu,%03lu words", (unsigned long)(words / 1000),
           (unsigned long)(words % 1000));
}

// Paints a list row's background. Rows sit flush against each other (ROW_GAP is
// 0, as in the design), so an unselected row needs a rule under it to read as a
// row at all; a selected one is a filled band and needs none. `last` suppresses
// the rule under the final row, which would otherwise look like a stray line.
static void drawRowBackground(GfxRenderer& r, const ScreenFrame& box, int bandTop,
                              int bandH, bool selected, bool last, int width = 0) {
  const bool tc = !darkMode;
  const int w = width > 0 ? width : box.w;
  if (selected) {
    // Rounded, as the design draws it. fillRoundedRect speaks in dither levels
    // rather than a bool, and Black/White are the two exact ends of that scale
    // (GfxRenderer.cpp:249) — so this inverts with dark mode like everything
    // else, without any halftone creeping into a solid band.
    r.fillRoundedRect(box.x, bandTop, w, bandH, ROW_RADIUS, tc ? Black : White);
  } else if (!last) {
    clippedFillRect(r, box.x, bandTop + bandH - ROW_RULE, w, ROW_RULE, tc);
  }
}

// ---------------------------------------------------------------------------
// The confirmation dialog
// ---------------------------------------------------------------------------

// One button of the pair. The selected one is a filled band, exactly like a
// selected list row; the other is an outline, so the box reads as two buttons
// and not as a label beside a band. Same corner radius as the rows.
//
// Note the two width conventions in GfxRenderer, which do not agree:
// drawRect's lineWidth overload paints x + width as its LAST column, while
// drawRoundedRect uses x + width - 1 like everything else. Rounded is used
// here, so the width passed is the real one.
static void drawPopupButton(GfxRenderer& r, int x, int y, int w, int h,
                            const char* label, bool selected) {
  const bool tc = !darkMode;
  if (selected) {
    r.fillRoundedRect(x, y, w, h, ROW_RADIUS, tc ? Black : White);
  } else {
    r.drawRoundedRect(x, y, w, h, 1, ROW_RADIUS, tc);
  }
  const int lw = textW(r, FONT_CHROME_M, label, EpdFontFamily::BOLD);
  int lx = x + (w - lw) / 2;
  if (lx < x) lx = x;
  drawClippedText(r, FONT_CHROME_M, lx,
                  textYInBandOf(inkOf(FONT_CHROME_M), y, h), label, w,
                  selected ? !tc : tc, EpdFontFamily::BOLD);
}

// Drawn last, on top of the list it is asking about: the box is small and
// centred, so the rows around it stay on screen. A modal that cleared the
// screen would be asking about something the reader can no longer see.
//
// `itemName` is the thing that will be destroyed — the note's title, the
// keyboard's name. It is what makes the question checkable, so it is drawn even
// when it has to truncate.
static void drawConfirmDialog(GfxRenderer& r, const ConfirmDialog& d,
                              const char* itemName) {
  if (!d.open()) return;
  const bool tc = !darkMode;

  const char* question = confirmQuestion(d.kind);
  const char* verb = confirmVerb(d.kind);
  static const char CANCEL[] = "Cancel";

  const TextInk qInk = inkOf(FONT_CHROME_L);
  const TextInk nInk = inkOf(FONT_CHROME_M);
  const int btnH = bandHeight(FONT_CHROME_M);

  const int cancelW = textW(r, FONT_CHROME_M, CANCEL, EpdFontFamily::BOLD)
                      + 2 * POPUP_BTN_PAD_X;
  const int verbW = textW(r, FONT_CHROME_M, verb, EpdFontFamily::BOLD)
                    + 2 * POPUP_BTN_PAD_X;
  const int btnRowW = cancelW + POPUP_BTN_GAP + verbW;

  int wantW = textW(r, FONT_CHROME_L, question, EpdFontFamily::BOLD);
  const int nameW = textW(r, FONT_CHROME_M, itemName);
  if (nameW > wantW) wantW = nameW;
  if (btnRowW > wantW) wantW = btnRowW;
  const int wantH = qInk.height() + LINE_LEAD + nInk.height() + POPUP_GAP + btnH;

  const PopupBox box = popupBoxOf(r.getScreenWidth(), r.getScreenHeight(),
                                  wantW, wantH);

  // Fill with the page colour first: the box has to hide the rows under it, or
  // the text of both would overprint.
  clippedFillRect(r, box.x, box.y, box.w, box.h, !tc);
  r.drawRoundedRect(box.x, box.y, box.w, box.h, POPUP_BORDER, ROW_RADIUS, tc);

  int y = box.innerY;
  drawClippedText(r, FONT_CHROME_L, box.innerX,
                  textYInBandOf(qInk, y, qInk.height()), question, box.innerW,
                  tc, EpdFontFamily::BOLD);
  y += qInk.height() + LINE_LEAD;
  drawClippedText(r, FONT_CHROME_M, box.innerX,
                  textYInBandOf(nInk, y, nInk.height()), itemName, box.innerW, tc);
  y += nInk.height() + POPUP_GAP;

  int bx = box.innerX + (box.innerW - btnRowW) / 2;
  if (bx < box.innerX) bx = box.innerX;
  drawPopupButton(r, bx, y, cancelW, btnH, CANCEL, !d.destructive);
  drawPopupButton(r, bx + cancelW + POPUP_BTN_GAP, y, verbW, btnH, verb,
                  d.destructive);
}

// The value column of a quick-menu row. Only the four cycling rows have one;
// the rest are actions and the column stays empty. Same job as the Settings
// value column, and deliberately the same words — the quick menu is a shortcut
// INTO those settings, not a second set of names for them.
static const char* quickValueText(QuickItem item) {
  switch (item) {
    case QuickItem::Typeface:
      return editorFont == EditorFont::MONO ? "Mono" : "Sans";
    case QuickItem::FontSize:
      switch (fontSize) {
        case FontSize::SMALL:  return "Small";
        case FontSize::MEDIUM: return "Medium";
        default:               return "Large";
      }
    case QuickItem::DarkMode:
      return darkMode ? "Dark" : "Light";
    case QuickItem::WritingMode:
      switch (writingMode) {
        case WritingMode::TYPEWRITER: return "Typewriter";
        case WritingMode::PAGINATION: return "Pagination";
        default:                      return "Normal";
      }
    default:
      return "";
  }
}

// The quick menu, drawn over whatever raised it. `subject` is the note it acts
// on — the row under the selector in the notes list, the open note in the
// editor — because four of the seven rows change a global and the other three
// do not, and only the title says which note the other three mean.
static void drawQuickMenu(GfxRenderer& r, const QuickMenu& m, const char* subject) {
  if (!m.open()) return;
  const bool tc = !darkMode;
  const int n = quickItemCount(m.kind);
  if (n <= 0) return;

  const TextInk tInk = inkOf(FONT_CHROME_L);
  const TextInk rInk = inkOf(FONT_CHROME_M);
  const int rowH = bandHeightOf(rInk);
  const int textPad = 12;   // inside the row band, so glyphs clear the fill

  int wantW = textW(r, FONT_CHROME_L, subject, EpdFontFamily::BOLD);
  for (int i = 0; i < n; i++) {
    const QuickItem item = quickItemAt(m.kind, i);
    int w = textW(r, FONT_CHROME_M, quickItemLabel(item), EpdFontFamily::BOLD)
            + 2 * textPad;
    const char* val = quickValueText(item);
    if (val[0]) w += DIGIT_GAP + textW(r, FONT_CHROME_M, val);
    if (w > wantW) wantW = w;
  }
  const int wantH = tInk.height() + POPUP_GAP + n * rowH;

  const PopupBox box = popupBoxOf(r.getScreenWidth(), r.getScreenHeight(),
                                  wantW, wantH);

  clippedFillRect(r, box.x, box.y, box.w, box.h, !tc);
  r.drawRoundedRect(box.x, box.y, box.w, box.h, POPUP_BORDER, ROW_RADIUS, tc);

  drawClippedText(r, FONT_CHROME_L, box.innerX,
                  textYInBandOf(tInk, box.innerY, tInk.height()), subject,
                  box.innerW, tc, EpdFontFamily::BOLD);

  const int listTop = box.innerY + tInk.height() + POPUP_GAP;
  for (int i = 0; i < n; i++) {
    const int bandTop = listTop + i * rowH;
    if (bandTop + rowH > box.y + box.h - POPUP_BORDER) break;   // clipped box
    const QuickItem item = quickItemAt(m.kind, i);
    const bool sel = (i == m.selection);

    if (sel) {
      r.fillRoundedRect(box.innerX, bandTop, box.innerW, rowH, ROW_RADIUS,
                        tc ? Black : White);
    }
    const int y = textYInBandOf(rInk, bandTop, rowH);

    const char* val = quickValueText(item);
    int rightEdge = box.innerX + box.innerW - textPad;
    int valW = 0;
    if (val[0]) {
      valW = textW(r, FONT_CHROME_M, val);
      drawRightText(r, FONT_CHROME_M, rightEdge, y, val, sel ? !tc : tc);
    }
    const int labelW = rightEdge - (valW ? valW + DIGIT_GAP : 0)
                       - (box.innerX + textPad);
    drawClippedText(r, FONT_CHROME_M, box.innerX + textPad, y,
                    quickItemLabel(item), labelW, sel ? !tc : tc,
                    sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }
}

// ===========================================================================
// Screen drawing functions
// ===========================================================================

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  FooterText ft;
  ft.left = "1-9: Jump";
  const ScreenFrame box = drawScreenFrame(renderer, gpio, "Ardosia", ft);
  const bool tc = !darkMode;

  // Section label above the list. The notes screen puts "[Esc] Back to Menu"
  // in this same slot: it is a context line, not a fixed title.
  const TextInk secInk = inkOf(FONT_CHROME_S);
  drawClippedText(renderer, FONT_CHROME_S, box.x,
                  textYInBandOf(secInk, box.top, secInk.height()),
                  "MAIN MENU", box.w, tc, EpdFontFamily::BOLD);

  // Menu items (base + dynamically detected OTA apps). This array is the screen
  // order, and must stay in step with the MENU_* indices in config.h. The last
  // one is read only while MENU_SHOW_SYNC is on — BASE_MENU_COUNT decides.
  static const char* baseMenuItems[] = {
      "Browse Files", "New Note", "Settings", "Hotspot", "Sync"};
  static_assert(BASE_MENU_COUNT <= (int)(sizeof(baseMenuItems) / sizeof(baseMenuItems[0])),
                "BASE_MENU_COUNT would read past the label array");
  const int menuCount = BASE_MENU_COUNT + otaAppCount;
  const int bandH = bandHeight(FONT_CHROME_L);
  const int pitch = rowPitch(FONT_CHROME_L);
  const int listTop = box.top + secInk.height() + WORKSPACE_PAD;

  // Scroll rather than clip. An earlier cut stopped drawing once a row would
  // not fit, which silently swallowed Hotspot and the CrossPoint entry at any
  // larger text size — exactly the kind of irreversible hiding this device
  // cannot afford, since there is no other way to reach a dual-boot slot.
  int maxVisible = (box.bottom - listTop) / pitch;
  if (maxVisible < 1) maxVisible = 1;
  const int startIdx = scrollStartOf(menuCount, maxVisible, mainMenuSelection);
  const int barW = drawListScrollbar(renderer, box, listTop, menuCount, maxVisible, startIdx);
  const int rowW = box.w - barW;

  const int digitW = digitColumnW(renderer);
  const int labelX = box.x + 16 + digitW + DIGIT_GAP;
  const int chevronW = renderer.getTextWidth(FONT_CHROME_L, CHEVRON, EpdFontFamily::BOLD);

  for (int i = startIdx; i < menuCount && (i - startIdx) < maxVisible; i++) {
    const int bandTop = listTop + (i - startIdx) * pitch;
    const int textY = textYInBand(FONT_CHROME_L, bandTop, bandH);
    const char* label = (i < BASE_MENU_COUNT) ? baseMenuItems[i]
                                              : otaApps[i - BASE_MENU_COUNT].name;
    const bool sel = (i == mainMenuSelection);
    const bool last = (i == menuCount - 1) || (i - startIdx) == maxVisible - 1;

    drawRowBackground(renderer, box, bandTop, bandH, sel, last, rowW);
    drawRowDigit(renderer, box.x + 16, bandTop, bandH, i, sel);

    // The chevron only marks the selected row, so only that row gives up the
    // width for it.
    const int labelW = box.x + rowW - 16 - labelX - (sel ? chevronW + DIGIT_GAP : 0);
    drawClippedText(renderer, FONT_CHROME_L, labelX, textY, label, labelW,
                    sel ? !tc : tc,
                    sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (sel) drawRowChevron(renderer, box.x + rowW - 16, bandTop, bandH, true);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  const bool tc = !darkMode;
  const int fc = noteVisibleCount();

  // The footer's left column doubles as the status line: a pending delete or an
  // active find filter takes it over, and it falls back to the hint otherwise.
  // The match count and the way out of a filter have to survive that move —
  // they are the only feedback the filter gives.
  char findBuf[80];
  FooterText ft;
  const bool asking = confirmDialog.open();
  if (asking || quickMenu.open()) {
    // While a box is up it swallows every other key, so the shortcut hints
    // would be naming things that do nothing. The footer shows the two ways
    // out instead, which is what neither box has room to spell.
    ft.left = "Enter:Choose  Esc:Cancel";
  } else if (noteFilterText()[0] != '\0') {
    snprintf(findBuf, sizeof(findBuf), "Find: %s  (%d)  Esc:Clear", noteFilterText(), fc);
    ft.left = findBuf;
    ft.centreWide = "Ctrl + N: Rename  |  Ctrl + D: Delete";
    ft.centreA = "Ctrl + N: Rename";
    ft.centreB = "Ctrl + D: Delete";
  } else {
    ft.left = "Type to Find";
    ft.centreWide = "Ctrl + N: Rename  |  Ctrl + D: Delete";
    ft.centreA = "Ctrl + N: Rename";
    ft.centreB = "Ctrl + D: Delete";
  }
  const ScreenFrame box = drawScreenFrame(renderer, gpio, "Notes", ft);

  // Context line, the slot the main menu fills with "MAIN MENU".
  const TextInk secInk = inkOf(FONT_CHROME_S);
  drawClippedText(renderer, FONT_CHROME_S, box.x,
                  textYInBandOf(secInk, box.top, secInk.height()),
                  "[Esc] Back to Menu", box.w, tc, EpdFontFamily::BOLD);

  const int bandH = bandHeight(FONT_CHROME_L);
  const int pitch = rowPitch(FONT_CHROME_L);
  const int listTop = box.top + secInk.height() + WORKSPACE_PAD;

  int maxVisible = (box.bottom - listTop) / pitch;
  if (maxVisible < 1) maxVisible = 1;
  const int startIdx = scrollStartOf(fc, maxVisible, selectedFileIndex);
  const int barW = drawListScrollbar(renderer, box, listTop, fc, maxVisible, startIdx);
  const int rowW = box.w - barW;
  const int chevronW = renderer.getTextWidth(FONT_CHROME_L, CHEVRON, EpdFontFamily::BOLD);

  if (fc == 0) {
    const char* empty = noteFilterText()[0] ? "No matching notes." : "No notes yet.";
    drawClippedText(renderer, FONT_CHROME_L, box.x, listTop, empty, box.w, tc);
    if (noteFilterText()[0] == '\0') {
      drawClippedText(renderer, FONT_CHROME_S, box.x, listTop + lineStep(FONT_CHROME_L),
                      "Press Ctrl+N to create one.", box.w, tc);
    }
  }

  for (int i = startIdx; i < fc && (i - startIdx) < maxVisible; i++) {
    FileInfo* note = noteVisibleAt(i);
    if (note == nullptr) continue;
    const int bandTop = listTop + (i - startIdx) * pitch;
    const int textY = textYInBand(FONT_CHROME_L, bandTop, bandH);

    const bool sel = (i == selectedFileIndex);
    const bool last = (i == fc - 1) || (i - startIdx) == maxVisible - 1;
    drawRowBackground(renderer, box, bandTop, bandH, sel, last, rowW);

    // Word count on the right, then the chevron on the selected row. The title
    // gets whatever is left, and truncates into it rather than over it.
    char wc[24];
    wordCountText(note->words, wc, sizeof(wc));
    const int wcW = renderer.getTextWidth(FONT_CHROME_M, wc);
    int rightEdge = box.x + rowW - 16;
    if (sel) {
      drawRowChevron(renderer, rightEdge, bandTop, bandH, true);
      rightEdge -= chevronW + DIGIT_GAP;
    }
    drawRightText(renderer, FONT_CHROME_M, rightEdge,
                  textYInBandOf(inkOf(FONT_CHROME_M), bandTop, bandH), wc,
                  sel ? !tc : tc);

    const int titleW = rightEdge - wcW - DIGIT_GAP - (box.x + 16);
    drawClippedText(renderer, FONT_CHROME_L, box.x + 16, textY, note->title, titleW,
                    sel ? !tc : tc,
                    sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  // Last, over the list. Both boxes name the note they CAPTURED when they
  // opened, not whatever the selection points at now.
  if (quickMenu.open()) {
    const FileInfo* subject = noteVisibleAt(quickMenu.index);
    drawQuickMenu(renderer, quickMenu, subject ? subject->title : "");
  }
  if (asking) {
    const FileInfo* doomed = noteVisibleAt(confirmDialog.index);
    drawConfirmDialog(renderer, confirmDialog, doomed ? doomed->title : "");
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Copy buf[from, to) into dest (capped) and return its pixel advance.
// The font paints only ink, so a selection band measures with the same
// advance the glyphs will use, or the highlight and the letters drift apart.
static int takeSpan(GfxRenderer& renderer, int fontId, const char* src,
                    int from, int to, char* dest, size_t cap) {
  dest[0] = '\0';
  if (to <= from || cap < 2) return 0;
  size_t n = (size_t)(to - from);
  if (n > cap - 1) n = cap - 1;
  memcpy(dest, src + from, n);
  dest[n] = '\0';
  utf8TrimPartialTail(dest);
  if (dest[0] == '\0') return 0;
  return renderer.getTextAdvanceX(fontId, dest);
}

// Helper: draw a single editor line from the buffer.
// The line is drawn normally first. A selected span is then covered by a
// solid band and redrawn in the opposite ink: drawText paints only the
// glyph, so white letters with no band would disappear on a white page.
static void drawEditorLine(GfxRenderer& renderer, int lineIdx, int x, int yPos,
                           int maxW, bool tc, int lineHeight) {
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();
  int totalLines = editorGetLineCount();

  int lineStart = editorGetLinePosition(lineIdx);
  int lineEnd = (lineIdx + 1 < totalLines) ? editorGetLinePosition(lineIdx + 1) : (int)bufLen;
  int dispEnd = lineEnd;
  if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

  int len = dispEnd - lineStart;
  if (len > 0) {
    char lineBuf[256];
    int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
    strncpy(lineBuf, buf + lineStart, copyLen);
    lineBuf[copyLen] = '\0';
    utf8TrimPartialTail(lineBuf);   // clamping to sizeof(lineBuf) may split a character
    drawClippedText(renderer, editorFontId(fontSize, editorFont), x, yPos, lineBuf, maxW, tc);
  }

  int selLo = 0, selHi = 0;
  if (!editorGetSelectionRange(&selLo, &selHi)) return;

  int ovLo = (selLo > lineStart) ? selLo : lineStart;
  int ovHi = (selHi < dispEnd) ? selHi : dispEnd;
  if (ovLo > dispEnd) ovLo = dispEnd;
  if (ovHi < ovLo) ovHi = ovLo;

  const int fontId = editorFontId(fontSize, editorFont);
  char span[256];

  if (ovHi > ovLo) {
    const int before = takeSpan(renderer, fontId, buf, lineStart, ovLo, span, sizeof(span));
    const int selW = takeSpan(renderer, fontId, buf, ovLo, ovHi, span, sizeof(span));
    int remain = maxW - before;
    if (remain < 0) remain = 0;
    int band = selW < remain ? selW : remain;
    if (band > 0 && span[0] != '\0') {
      clippedFillRect(renderer, x + before, yPos, band, lineHeight, tc);
      drawClippedText(renderer, fontId, x + before, yPos, span, remain, !tc);
    }
  } else if (selHi > dispEnd && selLo < lineEnd) {
    // Blank line, or a selected break sitting past the last glyph.
    const int before = takeSpan(renderer, fontId, buf, lineStart, dispEnd, span, sizeof(span));
    int spaceW = renderer.getSpaceWidth(fontId);
    if (spaceW < 2) spaceW = 8;
    clippedFillRect(renderer, x + before, yPos, spaceW, lineHeight, tc);
  }
}

// A dead key that is armed but not yet completed printed NOTHING on screen, and
// an e-ink refresh is slow enough that you cannot tell "it is thinking" from
// "it ignored me". So draw the waiting accent inside the cursor, in the
// inverted colour, where the letter it is about to modify will appear.
static void drawPendingDeadKey(GfxRenderer& r, int fontId, int x, int w,
                               int bandTop, int bandH, bool blockColor) {
  const uint32_t dead = inputGetPendingDeadKey();
  if (dead == 0) return;

  char mark[5];
  const int n = utf8Encode(dead, mark);
  if (n <= 0) return;
  mark[n] = '\0';

  int mw = r.getTextWidth(fontId, mark);
  if (mw < 0 || mw > w) mw = w;
  r.drawText(fontId, x + (w - mw) / 2, textYInBand(fontId, bandTop, bandH),
             mark, !blockColor);
}

// Helper: draw cursor at the given screen position
static void drawEditorCursor(GfxRenderer& renderer, int cursorY, int lineHeight,
                             int sw, bool tc) {
  int curLine = editorGetCursorLine();
  int curCol = editorGetCursorCol();
  char* buf = editorGetBuffer();

  int lineStart = editorGetLinePosition(curLine);
  char prefix[256];
  int prefixLen = (curCol < (int)sizeof(prefix) - 1) ? curCol : (int)sizeof(prefix) - 1;
  strncpy(prefix, buf + lineStart, prefixLen);
  prefix[prefixLen] = '\0';

  int cursorX = 10 + renderer.getTextAdvanceX(editorFontId(fontSize, editorFont), prefix);
  int cursorW = renderer.getSpaceWidth(editorFontId(fontSize, editorFont));
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 && cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, tc);
    drawPendingDeadKey(renderer, editorFontId(fontSize, editorFont), cursorX, cursorW,
                       cursorY, lineHeight, tc);
  }
}

// Get the mode indicator string for the current writing mode
static const char* getModeIndicator() {
  switch (writingMode) {
    case WritingMode::TYPEWRITER: return "[T]";
    case WritingMode::PAGINATION: return "[P]";
    default:                      return "[S]";
  }
}

// Helper: draw the standard editor header, returns textAreaTop
// centerText is optional text drawn centered in the header (e.g. "Page 1/3")
// The editor keeps a band of its own rather than the menus' frame: it is the
// writing surface, and a 65px header plus a 45px footer plus a border would
// cost a quarter of a landscape screen. What it does share is the language —
// the same faces, the same 2px rule, the same battery meter — and, like every
// other screen now, a height derived from the ink instead of a literal.
static constexpr int EDITOR_HEAD_PAD = 6;

static int editorHeaderHeight() {
  if (cleanMode) return 8;
  return EDITOR_HEAD_PAD + inkOf(FONT_CHROME_M).height() + EDITOR_HEAD_PAD + HEADER_RULE;
}

static int drawEditorHeader(GfxRenderer& renderer, HalGPIO& gpio, int sw, bool tc,
                            const char* centerText = nullptr) {
  if (cleanMode) return editorHeaderHeight();

  const TextInk titleInk = inkOf(FONT_CHROME_M);
  const int boxTop = EDITOR_HEAD_PAD;
  const int boxH = titleInk.height();
  const int titleY = textYInBandOf(titleInk, boxTop, boxH);
  const int smallY = textYInBandOf(inkOf(FONT_CHROME_S), boxTop, boxH);

  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorIsReadOnly()) {
    // Prefix so the warning stays visible when the title is long and clipped.
    snprintf(headerBuf, sizeof(headerBuf), "[read-only] %s", title);
  } else if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }
  // Battery first, at the right edge, so everything else knows where to stop.
  char batt[24];
  batteryText(gpio, batt, sizeof(batt));
  const int battW = renderer.getTextWidth(FONT_CHROME_S, batt, EpdFontFamily::BOLD);
  drawRightText(renderer, FONT_CHROME_S, sw - 10, smallY, batt, tc, EpdFontFamily::BOLD);

  // Mode indicator, right-anchored before the battery.
  const char* modeInd = getModeIndicator();
  int modeW = renderer.getTextAdvanceX(FONT_CHROME_S, modeInd);
  int modeX = sw - 10 - battW - DIGIT_GAP - modeW;
  drawClippedText(renderer, FONT_CHROME_S, modeX, smallY, modeInd, modeW + 5, tc);

  // Word count — drawn to the left of the mode indicator
  int titleMaxW = modeX - 10;
  if (showWordCount) {
    int wc = editorGetWordCount();
    char wcBuf[24];
    if (wc == 1) snprintf(wcBuf, sizeof(wcBuf), "1 word");
    else         snprintf(wcBuf, sizeof(wcBuf), "%d words", wc);
    int wcW = renderer.getTextAdvanceX(FONT_CHROME_S, wcBuf);
    int wcX = modeX - DIGIT_GAP - wcW;
    if (wcX > 10) {
      drawClippedText(renderer, FONT_CHROME_S, wcX, smallY, wcBuf, wcW + 5, tc);
      titleMaxW = wcX - 10;
    }
  }

  // Title — stops before word count (or mode indicator if word count hidden).
  // One size up from the rest: it is the note's name, the others are status.
  drawClippedText(renderer, FONT_CHROME_M, 10, titleY, headerBuf, titleMaxW, tc,
                  EpdFontFamily::BOLD);

  // Centered text (e.g. page indicator)
  if (centerText) {
    int ctW = renderer.getTextAdvanceX(FONT_CHROME_S, centerText);
    drawClippedText(renderer, FONT_CHROME_S, (sw - ctW) / 2, smallY, centerText, ctW + 5, tc);
  }
  const int h = editorHeaderHeight();
  clippedFillRect(renderer, 5, h - HEADER_RULE, sw - 10, HEADER_RULE, tc);
  return h;
}

// Both boxes, over whichever of the editor's three modes drew the page. They
// go in every one of them: a modal that only appeared in Normal mode would be
// invisible-but-live in Typewriter, which is a screen that swallows keys and
// shows no reason why.
static void drawEditorModals(GfxRenderer& r) {
  if (quickMenu.open()) drawQuickMenu(r, quickMenu, editorGetCurrentTitle());
  if (confirmDialog.open()) drawConfirmDialog(r, confirmDialog, editorGetCurrentTitle());
}

void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  int lineHeight = renderer.getLineHeight(editorFontId(fontSize, editorFont));
  if (lineHeight <= 0) lineHeight = 20;
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();

  // --- TYPEWRITER MODE ---
  if (writingMode == WritingMode::TYPEWRITER) {
    // In clean mode (Ctrl+Z): just text on blank screen, no header
    int textAreaTop = cleanMode ? 0 : drawEditorHeader(renderer, gpio, sw, tc);

    // Center the current line vertically
    int textAreaHeight = sh - textAreaTop;
    int centerY = textAreaTop + (textAreaHeight / 2) - (lineHeight / 2);

    // Draw only the current line
    if (curLine < totalLines) {
      drawEditorLine(renderer, curLine, 10, centerY, sw - 20, tc, lineHeight);
    }

    // Draw cursor
    drawEditorCursor(renderer, centerY, lineHeight, sw, tc);

    editorSetVisibleLines(1);

    drawEditorModals(renderer);
    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- PAGINATION MODE ---
  if (writingMode == WritingMode::PAGINATION) {
    // Pre-compute page info for the header
    // Use a temporary linesPerPage estimate (will be exact since header height is fixed)
    // Was a literal 38 here and a literal 38 returned by the header — two
    // copies of the same number, one of which would have gone stale the moment
    // the header changed size. One source now.
    int tempTextTop = editorHeaderHeight();
    int tempLinesPerPage = (sh - 5 - tempTextTop) / lineHeight;
    if (tempLinesPerPage < 1) tempLinesPerPage = 1;
    int currentPage = curLine / tempLinesPerPage;
    int totalPages = (totalLines + tempLinesPerPage - 1) / tempLinesPerPage;
    if (totalPages < 1) totalPages = 1;

    char pageStr[16];
    snprintf(pageStr, sizeof(pageStr), "Pg %d/%d", currentPage + 1, totalPages);
    int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc, pageStr);

    int textAreaBottom = sh - 5;
    int textAreaHeight = textAreaBottom - textAreaTop;
    int linesPerPage = textAreaHeight / lineHeight;
    if (linesPerPage < 1) linesPerPage = 1;

    // Recompute with actual linesPerPage if it differs
    currentPage = curLine / linesPerPage;
    int pageStart = currentPage * linesPerPage;

    editorSetVisibleLines(linesPerPage);

    // Draw lines for this page
    for (int i = 0; i < linesPerPage && (pageStart + i) < totalLines; i++) {
      int yPos = textAreaTop + (i * lineHeight);
      drawEditorLine(renderer, pageStart + i, 10, yPos, sw - 20, tc, lineHeight);
    }

    // Draw cursor if on this page
    if (curLine >= pageStart && curLine < pageStart + linesPerPage) {
      int cursorY = textAreaTop + ((curLine - pageStart) * lineHeight);
      drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
    }

    drawEditorModals(renderer);
    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- NORMAL MODE ---
  int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc);

  int textAreaBottom = sh - 5;
  int textAreaHeight = textAreaBottom - textAreaTop;
  int visibleLines = textAreaHeight / lineHeight;

  editorSetVisibleLines(visibleLines);

  int vpStart = editorGetViewportStart();

  // Draw visible lines
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int yPos = textAreaTop + (i * lineHeight);
    drawEditorLine(renderer, vpStart + i, 10, yPos, sw - 20, tc, lineHeight);
  }

  // Draw cursor
  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
    drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
  }

  drawEditorModals(renderer);
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  const bool tc = !darkMode;
  FooterText ft;
  ft.left = "Enter: Confirm";
  ft.centreWide = "Esc: Cancel";
  const ScreenFrame box = drawScreenFrame(renderer, gpio, renameScreenTitle(), ft);

  const TextInk secInk = inkOf(FONT_CHROME_S);
  drawClippedText(renderer, FONT_CHROME_S, box.x,
                  textYInBandOf(secInk, box.top, secInk.height()),
                  renameTargetIsApp() ? "MENU ENTRY" : "NOTE TITLE",
                  box.w, tc, EpdFontFamily::BOLD);

  const int boxY = box.top + secInk.height() + WORKSPACE_PAD;
  const int boxH = bandHeight(FONT_CHROME_L) + 4;
  const int textY = textYInBand(FONT_CHROME_L, boxY, boxH);
  renderer.drawRoundedRect(box.x, boxY, box.w, boxH, 1, 4, tc);
  drawClippedText(renderer, FONT_CHROME_L, box.x + 16, textY, renameBuffer, box.w - 32, tc);

  // Cursor — thin bar spanning the text's own ink, so it lines up with the
  // letters instead of with drawText()'s ascender-box origin.
  const TextInk titleInk = inkOf(FONT_CHROME_L);
  int cursorX = box.x + 16 + renderer.getTextAdvanceX(FONT_CHROME_L, renameBuffer);
  if (cursorX + 2 < box.x + box.w - 16) {
    if (inputGetPendingDeadKey() != 0) {
      // Armed accent: a filled block carrying the mark, same as in the editor.
      const int blockW = renderer.getSpaceWidth(FONT_CHROME_L) > 2
                         ? renderer.getSpaceWidth(FONT_CHROME_L) : 8;
      if (cursorX + blockW < box.x + box.w - 16) {
        renderer.fillRect(cursorX, boxY + 3, blockW, boxH - 6, tc);
        drawPendingDeadKey(renderer, FONT_CHROME_L, cursorX, blockW, boxY + 3, boxH - 6, tc);
      }
    } else {
      renderer.fillRect(cursorX, textY + titleInk.top, 2, titleInk.height(), tc);
    }
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  // The raw key readout goes in the middle column. HID usage codes identify a
  // key by POSITION, so this says what the keyboard actually sent, not what its
  // keycap claims — the only way to identify a layout without a serial console.
  // It is one indivisible string, so it never stacks: centreA/B stay null.
  char keyBuf[72];
  inputDescribeLastKey(keyBuf, sizeof(keyBuf));
  FooterText ft;

  // TEMPORARY: the button-ladder readout takes the left column while the
  // phantom-Up bug is being chased. See BUTTON_DIAG in main.cpp; removing it is
  // deleting this block and restoring the hint string.
  //   A2 <now> [<min>..<max>] up?<samples in the Up window> ev<Up events sent>
  // A min that dips into 1121..3800 while nothing was pressed, or an ev count
  // that grows on its own, is the proof the hypothesis needs.
  // The left column is narrow here — the key readout takes the middle — so it
  // says the one thing this screen does that no other does. U+2039/203A rather
  // than real arrows: the converted font's charset covers General Punctuation
  // (verified in the font header), not the arrow block.
  ft.left = "1-9:Jump  \xE2\x80\xB9\xE2\x80\xBA:Tabs  Esc:Back";
  ft.centreWide = keyBuf;

  // The phantom-Up hunt lives here, and says nothing until it catches one: an
  // Up event with a Down in its recent past. Only then does it take the middle
  // column, because that is the only moment the evidence is worth more than the
  // key readout. See BUTTON_DIAG in main.cpp.
  extern bool buttonDiagSuspect();
  extern void buttonDiagUpText(char* out, size_t n);
  char diagUpBuf[64];
  if (buttonDiagSuspect()) {
    buttonDiagUpText(diagUpBuf, sizeof(diagUpBuf));
    ft.centreWide = diagUpBuf;
  }
  const ScreenFrame box = drawScreenFrame(renderer, gpio, "Settings", ft);


  // Row geometry comes from the font: the band has to hold cap-top..descender.
  //
  // This used to squeeze the pitch to make nine rows fit, with a floor that
  // stopped it squeezing below the ink — so at the current text size the rows
  // simply ran past the bottom, overlapping the footer and cutting the last
  // one off. Nine rows do not fit a landscape screen at a readable size and no
  // amount of arithmetic changes that, so the list scrolls instead.
  // --- tab bar, from prototypes/settings-with-tabs.png ---
  //
  // The rows below are the open tab's rows, so the digits restart at 1 in each
  // tab and no tab is longer than the digits. That is what gave the eleventh
  // row its number back.
  // Drawn with the dialog's button helper on purpose: the design draws a tab
  // and a button as the same pill, filled when it is the one you are on.
  const int tabH = bandHeight(FONT_CHROME_S);

  // Three tiers, widest first. Measured against the real font rather than
  // estimated, because portrait has no room to spare. The three labels ink
  // 99 + 99 + 121 = 319px, so the bar comes to:
  //
  //   pad 28 / gap 20  ->  527px      pad 10 / gap 12  ->  403px
  //   short names at pad 10 / gap 12  ->  304px
  //
  // A landscape workspace is 760px wide and a PORTRAIT one is 440. So landscape
  // gets the roomy tier and portrait steps down to the tight one — never by
  // dropping a tab, which would take a whole group of settings out of reach on
  // a device whose only way into them is this screen. The short names are the
  // tier below that, for the day a tab gets a longer word.
  //
  // `fits` keeps one gap of clear space at the right rather than letting the
  // bar touch both edges, which is what an exact fit looks like on the panel.
  struct TabTier { bool shortName; int pad; int gap; };
  static const TabTier tiers[] = {{false, TAB_PAD_X, TAB_GAP},
                                  {false, TAB_PAD_X_TIGHT, TAB_GAP_TIGHT},
                                  {true, TAB_PAD_X_TIGHT, TAB_GAP_TIGHT}};
  int tier = 0;
  for (int t = 0; t < (int)(sizeof(tiers) / sizeof(tiers[0])); t++) {
    int total = 0;
    for (int i = 0; i < SETTINGS_TAB_COUNT; i++) {
      char probe[24];
      snprintf(probe, sizeof(probe), "F%d %s", i + 1,
               tiers[t].shortName ? settingsTabShortLabel(i) : settingsTabLabel(i));
      total += renderer.getTextWidth(FONT_CHROME_S, probe, EpdFontFamily::BOLD)
               + 2 * tiers[t].pad;
    }
    total += tiers[t].gap * (SETTINGS_TAB_COUNT - 1);
    tier = t;
    if (total <= box.w - tiers[t].gap) break;
  }

  int tabX = box.x;
  for (int t = 0; t < SETTINGS_TAB_COUNT; t++) {
    char label[24];
    snprintf(label, sizeof(label), "F%d %s", t + 1,
             tiers[tier].shortName ? settingsTabShortLabel(t) : settingsTabLabel(t));
    const int w = renderer.getTextWidth(FONT_CHROME_S, label, EpdFontFamily::BOLD)
                  + 2 * tiers[tier].pad;
    drawPopupButton(renderer, tabX, box.top, w, tabH, label, t == settingsTab);
    tabX += w + tiers[tier].gap;
  }

  const int bandH = bandHeight(FONT_CHROME_L);
  const int listTop = box.top + tabH + WORKSPACE_PAD;
  const int pitch = rowPitch(FONT_CHROME_L);

  // The open tab's rows, and the position inside it — settingsSelection is a
  // SET_* identity, not a screen position, so everything on screen goes through
  // the tab table.
  const int rowCount = settingsTabRowCount(settingsTab);
  const SettingsPlace place = settingsPlaceOf(settingsSelection);
  const int selPos = place.pos >= 0 ? place.pos : 0;

  int maxVisible = (box.bottom - listTop) / pitch;
  if (maxVisible < 1) maxVisible = 1;
  const int startIdx = scrollStartOf(rowCount, maxVisible, selPos);
  const int barW = drawListScrollbar(renderer, box, listTop, rowCount,
                                     maxVisible, startIdx);
  const int rowW = box.w - barW;
  const int digitW = digitColumnW(renderer);
  const int labelX = box.x + 16 + digitW + DIGIT_GAP;

  for (int p = startIdx; p < rowCount && (p - startIdx) < maxVisible; p++) {
    const int i = settingsTabRow(settingsTab, p);
    int bandTop = listTop + ((p - startIdx) * pitch);
    int yPos = textYInBand(FONT_CHROME_L, bandTop, bandH);
    bool sel = (i == settingsSelection);

    const bool lastRow = (p == rowCount - 1) || (p - startIdx) == maxVisible - 1;
    drawRowBackground(renderer, box, bandTop, bandH, sel, lastRow, rowW);
    // The digit is the row's place in the tab, which is what the key types.
    drawRowDigit(renderer, box.x + 16, bandTop, bandH, p, sel);

    // Value on the right
    char val[32] = "";
    if (i == SET_ORIENTATION) {
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      strcpy(val, "Portrait"); break;
        case Orientation::LANDSCAPE_CW:  strcpy(val, "Landscape CW"); break;
        case Orientation::PORTRAIT_INV:  strcpy(val, "Inverted"); break;
        case Orientation::LANDSCAPE_CCW: strcpy(val, "Landscape CCW"); break;
      }
    } else if (i == SET_DARK_MODE) {
      strcpy(val, darkMode ? "Dark" : "Light");
    } else if (i == SET_WRITING) {
      switch (writingMode) {
        case WritingMode::NORMAL:     strcpy(val, "Normal"); break;
        case WritingMode::TYPEWRITER: strcpy(val, "Typewriter"); break;
        case WritingMode::PAGINATION: strcpy(val, "Pagination"); break;
      }
    } else if (i == SET_FONT_SIZE) {
      switch (fontSize) {
        case FontSize::SMALL:  strcpy(val, "Small"); break;
        case FontSize::MEDIUM: strcpy(val, "Medium"); break;
        default:               strcpy(val, "Large"); break;
      }
    } else if (i == SET_EDITOR_FONT) {
      strcpy(val, editorFont == EditorFont::MONO ? "Mono" : "Sans");
    } else if (i == SET_KEYBOARD) {
      switch (keyboardLayout) {
        case KeyboardLayout::ABNT2:   strcpy(val, "ABNT2");   break;
        case KeyboardLayout::US_INTL: strcpy(val, "US-Intl"); break;
        default:                      strcpy(val, "US");      break;
      }
    } else if (i == SET_SLEEP) {
      switch (sleepScreenMode) {
        case SleepScreenMode::SLIDESHOW: strcpy(val, "Slideshow"); break;
        case SleepScreenMode::SHUFFLE:   strcpy(val, "Shuffle");   break;
        default:                         strcpy(val, "Text");      break;
      }
    } else if (i == SET_NOTE_ORDER) {
      switch (noteSort) {
        case NoteSort::ALPHA_DESC: strcpy(val, "Z-A"); break;
        case NoteSort::NEWEST:     strcpy(val, "Newest"); break;
        case NoteSort::OLDEST:     strcpy(val, "Oldest"); break;
        default:                   strcpy(val, "A-Z"); break;
      }
    } else if (i == SET_DPAD) {
      strcpy(val, dpadMode == DpadMode::Natural ? "Natural" : "Standard");
    } else if (i == SET_PAIRED_KB) {
      int kbCount = getPairedKeyboardCount();
      if (kbCount == 0) strcpy(val, "None");
      else if (kbCount == 1) strcpy(val, "1 keyboard");
      else snprintf(val, sizeof(val), "%d keyboards", kbCount);
    }

    // Value on the right in the secondary size, the same weight the notes list
    // gives its word count. The label takes what is left.
    const int valW = val[0] ? renderer.getTextWidth(FONT_CHROME_M, val) : 0;
    if (val[0] != '\0') {
      drawRightText(renderer, FONT_CHROME_M, box.x + rowW - 16,
                    textYInBandOf(inkOf(FONT_CHROME_M), bandTop, bandH), val,
                    sel ? darkMode : !darkMode);
    }
    const int labelW = box.x + rowW - 16 - (valW ? valW + DIGIT_GAP : 0) - labelX;
    drawClippedText(renderer, FONT_CHROME_L, labelX, yPos, settingLabel(i), labelW,
                    sel ? darkMode : !darkMode,
                    sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  const bool tc = !darkMode;
  FooterText ft;
  ft.left = "Enter:Connect  Right:Scan";
  ft.centreWide = "Left:Disconnect  |  Esc:Back";
  ft.centreA = "Left:Disconnect";
  ft.centreB = "Esc:Back";
  const ScreenFrame box = drawScreenFrame(renderer, gpio, "Bluetooth Devices", ft);
  const int sw = renderer.getScreenWidth();

  // The whole screen is stacked from a running cursor.  The status line, the
  // scan/passkey block and the device list each vary in height, and the fixed
  // y values this used before (45, 60, 72, 104) were closer together than a
  // line of text is tall — so as soon as more than one of them was visible,
  // which is exactly while a scan is running, they printed on top of each
  // other and the whole screen bunched up against the header.
  int y = box.top;
  const int smallStep = lineStep(FONT_CHROME_S);

  // Connection status (left) and the stored pairing (right) share one line.
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "Connected to keyboard"; break;
    case BLEState::SCANNING:     status = "Scanning for devices..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "Not connected"; break;
  }
  drawClippedText(renderer, FONT_CHROME_S, 10, y, status, sw / 2 - 20, tc);

  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName)) {
    char pairedStr[64];
    snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
    drawClippedText(renderer, FONT_CHROME_S, sw / 2, y, pairedStr, sw / 2 - 10, tc);
  }
  y += smallStep;

  // Pairing code takes over the block; otherwise show scan progress.
  uint32_t passkey = getCurrentPasskey();
  if (passkey > 0) {
    y += 10;
    drawClippedText(renderer, FONT_CHROME_L, 20, y, "PAIRING CODE:", 0, tc, EpdFontFamily::BOLD);
    y += lineStep(FONT_CHROME_L);
    char passkeyStr[32];
    snprintf(passkeyStr, sizeof(passkeyStr), "%06lu", passkey);
    drawClippedText(renderer, FONT_CHROME_XL, 20, y, passkeyStr, 0, tc, EpdFontFamily::BOLD);
    y += lineStep(FONT_CHROME_XL) + 4;
    drawClippedText(renderer, FONT_CHROME_S, 20, y, "Type this code on your keyboard", 0, tc);
    y += smallStep;
    drawClippedText(renderer, FONT_CHROME_S, 20, y, "then press Enter", 0, tc);
    y += smallStep;
  } else if (isDeviceScanning()) {
    static uint8_t dotPhase = 0;
    static uint32_t lastAnimMs = 0;
    if (millis() - lastAnimMs > 900) {
      dotPhase = (dotPhase + 1) % 4;
      lastAnimMs = millis();
    }
    std::string dots(dotPhase, '.');
    char scanningStr[64];
    snprintf(scanningStr, sizeof(scanningStr), "Searching for devices%s", dots.c_str());
    drawClippedText(renderer, FONT_CHROME_S, 10, y, scanningStr, sw / 2 - 20, tc);

    char foundStr[32];
    snprintf(foundStr, sizeof(foundStr), "Found: %d", getDiscoveredDeviceCount());
    drawClippedText(renderer, FONT_CHROME_S, sw / 2, y, foundStr, sw / 2 - 10, tc);
    y += smallStep;
  }

  const int ruleY = box.bottom;

  // Device list
  int deviceCount = getDiscoveredDeviceCount();
  if (deviceCount > 0) {
    BleDeviceInfo* devices = getDiscoveredDevices();

    y += 8;
    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "Available devices: %d", deviceCount);
    drawClippedText(renderer, FONT_CHROME_S, 10, y, headerStr, 0, tc, EpdFontFamily::BOLD);
    y += smallStep;

    const int bandH = bandHeight(FONT_CHROME_L);
    const int pitch = rowPitch(FONT_CHROME_L);
    const int listTop = y;

    // How many rows actually fit between here and the footer rule.  The old
    // fixed 10 was independent of both the font and where the list started.
    int maxDevicesToShow = (ruleY - 6 - listTop) / pitch;
    if (maxDevicesToShow < 1) maxDevicesToShow = 1;

    const int startIndex =
        scrollStartOf(deviceCount, maxDevicesToShow, bluetoothDeviceSelection);
    int devicesToShow = (deviceCount - startIndex < maxDevicesToShow)
                        ? deviceCount - startIndex : maxDevicesToShow;

    for (int i = 0; i < devicesToShow; i++) {
      int deviceIndex = startIndex + i;
      int bandTop = listTop + (i * pitch);
      int textY = textYInBand(FONT_CHROME_L, bandTop, bandH);

      bool isSelected = (bluetoothDeviceSelection == deviceIndex);
      bool isConnected = (getCurrentDeviceAddress() == devices[deviceIndex].address);

      const char* displayName = devices[deviceIndex].name.empty()
                                ? devices[deviceIndex].address.c_str()
                                : devices[deviceIndex].name.c_str();

      // Tag what the advertising payload says this thing is, so a keyboard is
      // distinguishable from the phones and earbuds a scan also picks up.
      const uint16_t app = devices[deviceIndex].appearance;
      const char* kindTag = "";
      if (app == BLE_APPEARANCE_KEYBOARD)                 kindTag = "[KBD] ";
      else if (app == BLE_APPEARANCE_MOUSE)               kindTag = "[MOU] ";
      else if ((app >> 6) == BLE_APPEARANCE_CAT_HID)      kindTag = "[HID] ";
      else if (devices[deviceIndex].isHid)                kindTag = "[HID] ";

      char deviceLabel[96];
      snprintf(deviceLabel, sizeof(deviceLabel), "%s%s", kindTag, displayName);

      const bool hot = (isSelected || isConnected);
      drawRowBackground(renderer, box, bandTop, bandH, hot,
                        i == devicesToShow - 1, box.w);

      // RSSI on the right in the secondary size, then the name in what is left.
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%ddBm", devices[deviceIndex].rssi);
      const int rssiW = renderer.getTextWidth(FONT_CHROME_M, rssiStr);
      drawRightText(renderer, FONT_CHROME_M, box.x + box.w - 16,
                    textYInBandOf(inkOf(FONT_CHROME_M), bandTop, bandH), rssiStr,
                    hot ? !tc : tc);

      const int nameMaxW = box.w - 32 - rssiW - DIGIT_GAP;
      drawClippedText(renderer, FONT_CHROME_L, box.x + 16, textY, deviceLabel, nameMaxW,
                      hot ? !tc : tc,
                      hot ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }

    // Position indicator.  The list scrolls as a sliding window (the selection
    // is kept in view), so report the visible range rather than a page number,
    // which never matched what was on screen.
    if (deviceCount > devicesToShow) {
      char navHint[40];
      snprintf(navHint, sizeof(navHint), "%d-%d of %d",
               startIndex + 1, startIndex + devicesToShow, deviceCount);
      int navY = listTop + (devicesToShow * pitch);
      if (navY + smallStep < ruleY - 2)
        drawClippedText(renderer, FONT_CHROME_S, 15, navY, navHint, 0, tc);
    }
  } else {
    y += 10;
    drawClippedText(renderer, FONT_CHROME_L, 20, y, "No devices found", 0, tc);
    y += lineStep(FONT_CHROME_L);
    drawClippedText(renderer, FONT_CHROME_S, 20, y, "Press Enter to scan for devices", 0, tc);
  }


  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawPairedKeyboardsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  const bool tc = !darkMode;
  const bool asking = confirmDialog.open();
  FooterText ft;
  if (asking) {
    ft.left = "Enter:Choose  Esc:Cancel";
  } else {
    ft.left = "Enter:Connect  D:Forget";
    ft.centreWide = "Left:Disconnect  |  Esc:Back";
    ft.centreA = "Left:Disconnect";
    ft.centreB = "Esc:Back";
  }
  const ScreenFrame box = drawScreenFrame(renderer, gpio, "Paired Keyboards", ft);

  int count = getPairedKeyboardCount();
  if (count == 0) {
    drawClippedText(renderer, FONT_CHROME_L, box.x, box.top, "No paired keyboards", box.w, tc);
    drawClippedText(renderer, FONT_CHROME_S, box.x, box.top + lineStep(FONT_CHROME_L),
                    "Go to Bluetooth to scan and connect", box.w, tc);
  } else {
    std::string currentAddr = getCurrentDeviceAddress();
    const int bandH = bandHeight(FONT_CHROME_L);
    const int pitch = rowPitch(FONT_CHROME_L);
    const int listTop = box.top;

    for (int i = 0; i < count; i++) {
      std::string addr, name; uint8_t addrType;
      getPairedKeyboard(i, addr, name, addrType);

      int bandTop = listTop + (i * pitch);
      int textY = textYInBand(FONT_CHROME_L, bandTop, bandH);
      bool sel = (i == pairedKeyboardSelection);
      bool active = (!currentAddr.empty() && currentAddr == addr);

      drawRowBackground(renderer, box, bandTop, bandH, sel, i == count - 1);

      // Tag first, so the name knows what is left. Secondary size, like the
      // word count in the notes list and the value column in Settings.
      const char* tag = nullptr;
      if (active) tag = "active";
      else if (i == getLastUsedKeyboardIndex() && currentAddr.empty()) tag = "last";

      const int tagW = tag ? renderer.getTextWidth(FONT_CHROME_M, tag) : 0;
      if (tag) {
        drawRightText(renderer, FONT_CHROME_M, box.x + box.w - 16,
                      textYInBandOf(inkOf(FONT_CHROME_M), bandTop, bandH), tag,
                      sel ? !tc : tc);
      }
      const int nameW = box.w - 32 - (tagW ? tagW + DIGIT_GAP : 0);
      drawClippedText(renderer, FONT_CHROME_L, box.x + 16, textY, name.c_str(), nameW,
                      sel ? !tc : tc,
                      sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }
  }

  // `D` used to remove a bond on the spot, with no question — on the one screen
  // you reach because the keyboard is already misbehaving.
  if (asking) {
    std::string addr, name; uint8_t addrType;
    if (confirmDialog.index >= 0 && confirmDialog.index < count) {
      getPairedKeyboard(confirmDialog.index, addr, name, addrType);
    }
    drawConfirmDialog(renderer, confirmDialog, name.c_str());
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Helper: draw signal strength indicator (1-4 bars)
static void drawSignalBars(GfxRenderer& r, int x, int y, int rssi, bool color) {
  // y is the top of the 13px-tall bar group; callers centre it in the row band.
  // RSSI to bars: > -50 = 4, > -65 = 3, > -75 = 2, else 1
  int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 3;  // bar heights: 4, 7, 10, 13
    int by = y + 13 - bh;
    if (i < bars) {
      clippedFillRect(r, x + i * 5, by, 3, bh, color);
    } else {
      clippedFillRect(r, x + i * 5, by + bh - 2, 3, 2, color);
    }
  }
}

void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  const bool tc = !darkMode;
  const SyncState state = getSyncState();

  // The footer is built before the frame is drawn, because the frame needs to
  // know how tall it is. Buffers live out here for the same reason: FooterText
  // only holds pointers.
  char sentBuf[32];
  FooterText ft;
  switch (state) {
    case SyncState::SCANNING:
      ft.left = "Esc: Cancel";
      break;
    case SyncState::NETWORK_LIST:
      ft.left = "*=encrypted  +=saved";
      ft.centreWide = "Enter: Select  |  Esc: Back";
      ft.centreA = "Enter: Select";
      ft.centreB = "Esc: Back";
      break;
    case SyncState::PASSWORD_ENTRY:
      ft.left = "Enter: Connect";
      ft.centreWide = "Esc: Cancel";
      break;
    case SyncState::CONNECTING:
      ft.left = "Esc: Cancel";
      break;
    case SyncState::SYNCING:
      snprintf(sentBuf, sizeof(sentBuf), "Sent: %d", getSyncFilesSent());
      ft.left = sentBuf;
      ft.centreWide = "Esc: Cancel";
      break;
    case SyncState::AP_ACTIVE:
      ft.left = "Esc: Stop";
      break;
    case SyncState::DONE:
      ft.left = "Returning to menu...";
      break;
    case SyncState::CONNECT_FAILED:
      ft.left = "Enter: Retry";
      ft.centreWide = "Esc: Back";
      break;
    case SyncState::SAVE_PROMPT:
    case SyncState::FORGET_PROMPT:
      ft.left = "Enter/Up: Yes";
      ft.centreWide = "Down/Esc: No";
      break;
  }

  const ScreenFrame box =
      drawScreenFrame(renderer, gpio, state == SyncState::AP_ACTIVE ? "Hotspot" : "Sync", ft);

  // A running cursor for the states that are simply a stack of lines. This
  // replaces the fixed y values (50, 80, 120, 145) the screen used before —
  // the same pattern that had the Bluetooth screen printing lines over each
  // other once more than one block was visible at a time.
  int y = box.top;
  auto line = [&](int font, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
    const TextInk ink = inkOf(font);
    drawClippedText(renderer, font, box.x, textYInBandOf(ink, y, ink.height()), text, box.w, tc,
                    style);
    y += lineStep(font);
  };

  switch (state) {
    case SyncState::SCANNING:
      line(FONT_CHROME_L, "Scanning for networks...");
      break;

    case SyncState::NETWORK_LIST: {
      const int nc = getNetworkCount();
      const int sel = getSelectedNetwork();

      if (nc == 0) {
        const char* st = getSyncStatusText();
        line(FONT_CHROME_L, st[0] ? st : "No networks found");
        line(FONT_CHROME_S, "Enter rescans.");
        break;
      }

      // Context line, then the list — the same shape the main menu and the notes
      // browser use, so the three read as one family.
      const TextInk secInk = inkOf(FONT_CHROME_S);
      drawClippedText(renderer, FONT_CHROME_S, box.x,
                      textYInBandOf(secInk, box.top, secInk.height()),
                      "SELECT NETWORK", box.w, tc, EpdFontFamily::BOLD);

      const int bandH = bandHeight(FONT_CHROME_L);
      const int pitch = rowPitch(FONT_CHROME_L);
      const int listTop = box.top + secInk.height() + WORKSPACE_PAD;
      int maxVisible = (box.bottom - listTop) / pitch;
      if (maxVisible < 1) maxVisible = 1;
      const int startIdx = scrollStartOf(nc, maxVisible, sel);
      const int barW = drawListScrollbar(renderer, box, listTop, nc, maxVisible, startIdx);
      const int rowW = box.w - barW;

      for (int i = startIdx; i < nc && (i - startIdx) < maxVisible; i++) {
        const int bandTop = listTop + (i - startIdx) * pitch;
        const int textY = textYInBand(FONT_CHROME_L, bandTop, bandH);
        const bool isSel = (i == sel);
        const bool last = (i == nc - 1) || (i - startIdx) == maxVisible - 1;

        char label[48];
        snprintf(label, sizeof(label), "%s%s%s",
                 isNetworkEncrypted(i) ? "* " : "  ",
                 isNetworkSaved(i) ? "+ " : "",
                 getNetworkSSID(i));

        // Four 3px bars on a 5px pitch: 18px wide, ending at the same 16px
        // inset every other right-hand column uses.
        static constexpr int BARS_W = 18;
        const int barsX = box.x + rowW - 16 - BARS_W;

        drawRowBackground(renderer, box, bandTop, bandH, isSel, last, rowW);
        drawSignalBars(renderer, barsX, bandTop + (bandH - 13) / 2,
                       getNetworkRSSI(i), isSel ? !tc : tc);
        drawClippedText(renderer, FONT_CHROME_L, box.x + 16, textY, label,
                        barsX - DIGIT_GAP - (box.x + 16), isSel ? !tc : tc,
                        isSel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      }
      break;
    }

    case SyncState::PASSWORD_ENTRY: {
      char heading[64];
      snprintf(heading, sizeof(heading), "Password for %s", getNetworkSSID(getSelectedNetwork()));
      line(FONT_CHROME_S, heading, EpdFontFamily::BOLD);

      const int boxY = y + WORKSPACE_PAD;
      const int boxH = bandHeight(FONT_CHROME_L) + 4;
      renderer.drawRoundedRect(box.x, boxY, box.w, boxH, 1, ROW_RADIUS, tc);

      // Dots, not the password itself.
      const int pLen = getPasswordLen();
      char dots[80];
      const int shown = pLen < (int)sizeof(dots) - 1 ? pLen : (int)sizeof(dots) - 1;
      for (int i = 0; i < shown; i++) dots[i] = '*';
      dots[shown] = '\0';

      const int dotsY = textYInBand(FONT_CHROME_L, boxY, boxH);
      drawClippedText(renderer, FONT_CHROME_L, box.x + 16, dotsY, dots, box.w - 32, tc);

      const TextInk pwInk = inkOf(FONT_CHROME_L);
      int cursorX = box.x + 16 + renderer.getTextAdvanceX(FONT_CHROME_L, dots);
      int cursorW = renderer.getSpaceWidth(FONT_CHROME_L);
      if (cursorW < 2) cursorW = 8;
      if (cursorX + cursorW < box.x + box.w - 16)
        renderer.fillRect(cursorX, dotsY + pwInk.top, cursorW, pwInk.height(), tc);
      break;
    }

    case SyncState::CONNECTING:
      line(FONT_CHROME_L, getSyncStatusText());
      break;

    case SyncState::AP_ACTIVE: {
      char buf[72];
      snprintf(buf, sizeof(buf), "SSID: %s", getApSsid());        line(FONT_CHROME_L, buf);
      snprintf(buf, sizeof(buf), "Pass: %s", getApPassword());    line(FONT_CHROME_L, buf);
      snprintf(buf, sizeof(buf), "IP: %s", getSyncStatusText());  line(FONT_CHROME_L, buf);
      y += WORKSPACE_PAD;
      snprintf(buf, sizeof(buf), "Clients: %d", getApStationCount());     line(FONT_CHROME_M, buf);
      snprintf(buf, sizeof(buf), "Received: %d", getSyncFilesReceived()); line(FONT_CHROME_M, buf);
      break;
    }

    case SyncState::SYNCING: {
      line(FONT_CHROME_L, getSyncStatusText(), EpdFontFamily::BOLD);
      y += WORKSPACE_PAD;

      const bool pcConn = isPcConnected();
      const int sent = getSyncFilesSent();

      // Static so the pointers pushed below stay valid past this scope.
      static char fileStageText[MAX_FILES][16];
      static struct { const char* text; bool done; } stages[MAX_FILES + 3];
      int numStages = 0;
      auto push = [&](const char* text, bool done) {
        if (numStages < MAX_FILES + 3) stages[numStages++] = {text, done};
      };

      push("Connected to WiFi", true);
      push("PC connected", pcConn);
      if (pcConn) {
        for (int i = 0; i < sent && i < MAX_FILES; i++) {
          snprintf(fileStageText[i], sizeof(fileStageText[i]), "File %d sent", i + 1);
          push(fileStageText[i], true);
        }
        push("Sync complete", false);
      }

      // Keep the last finished stage and the one after it in view.
      const int lineH = lineStep(FONT_CHROME_M);
      const int listTop = y;
      int maxVisible = (box.bottom - listTop) / lineH;
      if (maxVisible < 1) maxVisible = 1;
      int lastDone = 0;
      for (int i = 0; i < numStages; i++) {
        if (stages[i].done) lastDone = i;
      }
      const int startIdx = scrollStartOf(numStages, maxVisible,
                                         lastDone + 1 < numStages ? lastDone + 1 : lastDone);

      for (int i = startIdx; i < numStages && (i - startIdx) < maxVisible; i++) {
        char buf[52];
        snprintf(buf, sizeof(buf), "%s %s", stages[i].done ? "[x]" : "[-]", stages[i].text);
        const TextInk ink = inkOf(FONT_CHROME_M);
        drawClippedText(renderer, FONT_CHROME_M, box.x,
                        textYInBandOf(ink, listTop + (i - startIdx) * lineH, ink.height()),
                        buf, box.w, tc);
      }
      break;
    }

    case SyncState::DONE:
      line(FONT_CHROME_S, "SYNC COMPLETE", EpdFontFamily::BOLD);
      y += WORKSPACE_PAD;
      line(FONT_CHROME_L, getSyncStatusText());
      break;

    case SyncState::CONNECT_FAILED:
      line(FONT_CHROME_L, "Connection failed");
      break;

    case SyncState::SAVE_PROMPT:
      line(FONT_CHROME_S, "CONNECTED", EpdFontFamily::BOLD);
      y += WORKSPACE_PAD;
      line(FONT_CHROME_L, getSyncStatusText());
      y += WORKSPACE_PAD;
      line(FONT_CHROME_L, "Save password?", EpdFontFamily::BOLD);
      break;

    case SyncState::FORGET_PROMPT:
      line(FONT_CHROME_L, "Saved password failed");
      y += WORKSPACE_PAD;
      line(FONT_CHROME_L, "Forget saved password?", EpdFontFamily::BOLD);
      break;
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

