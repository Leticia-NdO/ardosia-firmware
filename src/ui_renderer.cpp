#include "ui_renderer.h"
#include "config.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "input_handler.h"
#include "utf8_util.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalDisplay.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern bool deleteConfirmPending;
extern WritingMode writingMode;
extern FontSize fontSize;
extern KeyboardLayout keyboardLayout;
extern SleepScreenMode sleepScreenMode;
extern SleepBrightness sleepBrightness;
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
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>

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

static EpdFont u10Regular(&ubuntu_10_regular);
static EpdFont u10Bold(&ubuntu_10_bold);
static EpdFontFamily u10Family(&u10Regular, &u10Bold);

// OTA app detection (defined in main.cpp)
extern OtaAppEntry otaApps[];
extern int otaAppCount;

// Extern shared state (defined in main.cpp)
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int bluetoothDeviceSelection;
extern int pairedKeyboardSelection;
extern Orientation currentOrientation;
extern int charsPerLine;
extern char renameBuffer[];
extern int renameBufferLen;

void rendererSetup(GfxRenderer& renderer) {
  renderer.insertFont(FONT_LARGE, ns16Family);
  renderer.insertFont(FONT_BODY, ns14Family);
  renderer.insertFont(FONT_UI, ns12Family);
  renderer.insertFont(FONT_SMALL, u10Family);
}

// ---------------------------------------------------------------------------
// Vertical metrics
//
// GfxRenderer::drawText(font, x, y, ...) treats `y` as the top of the font's
// ASCENDER BOX and puts the baseline at y + ascender.  That ascender is the
// tallest glyph anywhere in the font (27px in notosans_12), while ordinary text
// only reaches cap height (18px).  So a label drawn at the top of a highlight
// band sits ~9px lower than it looks like it should, and its descenders land
// 33px below `y` — past the bottom of a 35px band.  That is precisely the
// "words sit low and get clipped along the bottom" symptom.
//
// Fix: measure the band the text actually paints — the top of an accented
// capital (Á, the tallest thing Portuguese text produces) down to the bottom of
// a descender (g) — straight out of the font tables, and centre THAT.
//
// Using Á rather than A on purpose: pt-br support is the next project, and a
// layout tuned to unaccented ASCII would clip every "á" and "ç" the moment it
// lands.
// ---------------------------------------------------------------------------

// Ink extents of a line of text, measured downward from drawText()'s `y`.
struct TextInk {
  int top;
  int bottom;
  int height() const { return bottom - top; }
};

static const EpdFont& fontFor(int fontId) {
  if (fontId == FONT_LARGE) return ns16Regular;
  if (fontId == FONT_BODY)  return ns14Regular;
  if (fontId == FONT_UI)    return ns12Regular;
  return u10Regular;
}

static TextInk inkOf(int fontId) {
  const EpdFont& f = fontFor(fontId);
  const int asc = f.data->ascender;

  const EpdGlyph* tall = f.getGlyph(0x00C1);   // 'Á'
  if (!tall) tall = f.getGlyph('A');
  const EpdGlyph* low = f.getGlyph('g');

  TextInk ink;
  ink.top    = tall ? asc - tall->top : 0;
  // descender depth = how far the glyph bitmap hangs below the baseline
  ink.bottom = asc + (low ? low->height - low->top : -f.data->descender);
  return ink;
}

// A selectable row = the highlight band, plus the gap to the next row.
static constexpr int ROW_PAD = 3;   // clear space above/below the ink in a band
static constexpr int ROW_GAP = 5;   // space between one band and the next

static int bandHeight(int fontId) { return inkOf(fontId).height() + 2 * ROW_PAD; }
static int rowPitch(int fontId)   { return bandHeight(fontId) + ROW_GAP; }

// Distance between successive lines of plain (unbanded) stacked text.
// Not getLineHeight(): that returns the font's advanceY, which for ubuntu_10 is
// exactly the ink height, so stacked lines come out touching.
static int lineStep(int fontId) { return inkOf(fontId).height() + 3; }

// The `y` to hand drawText() so that its ink is centred in [bandTop, bandTop+bandH).
static int textYInBand(int fontId, int bandTop, int bandH) {
  const TextInk ink = inkOf(fontId);
  int y = bandTop + (bandH - ink.height()) / 2 - ink.top;
  if (bandH >= ink.height()) {
    // Band can hold the text: keep every pixel of ink inside it.
    const int yMin = bandTop - ink.top;
    const int yMax = bandTop + bandH - ink.bottom;
    if (y < yMin) y = yMin;
    if (y > yMax) y = yMax;
  }
  return y;
}

// Footer: a horizontal rule with `lines` hint lines of FONT_SMALL below it.
static constexpr int FOOTER_GAP = 8;   // rule -> first hint line

static int footerRuleY(GfxRenderer& r, int lines) {
  return r.getScreenHeight() - (FOOTER_GAP + lines * lineStep(FONT_SMALL) + 4);
}
static int footerLineY(int ruleY, int index) {
  return ruleY + FOOTER_GAP + index * lineStep(FONT_SMALL);
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

// Safe line — just clamp to screen
static void clippedLine(GfxRenderer& r, int x1, int y1, int x2, int y2,
                        bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  // Clamp rather than reject
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  x1 = clamp(x1, 0, sw - 1);
  x2 = clamp(x2, 0, sw - 1);
  y1 = clamp(y1, 0, sh - 1);
  y2 = clamp(y2, 0, sh - 1);
  r.drawLine(x1, y1, x2, y2, state);
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
// Helper: draw battery percentage in top-right
// ---------------------------------------------------------------------------
static void drawBattery(GfxRenderer& renderer, HalGPIO& gpio) {
  int pct = gpio.getBatteryPercentage();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  drawRightText(renderer, FONT_SMALL, renderer.getScreenWidth() - 8, 5, buf, !darkMode);
}

// Helper: draw BLE status
static void drawBleStatus(GfxRenderer& renderer, int x, int y) {
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "KB Connected"; break;
    case BLEState::SCANNING:     status = "Scanning..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "KB Disconnected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, x, y, status, 0, !darkMode);
}

// ===========================================================================
// Screen drawing functions
// ===========================================================================

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;  // text color

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Title
  renderer.drawCenteredText(FONT_BODY, 30, "Ardosia", tc, EpdFontFamily::BOLD);

  // Menu items (base + dynamically detected OTA apps)
  static const char* baseMenuItems[] = {
      "Browse Files", "New Note", "Settings", "Sync", "Sync (Hotspot)"};
  int menuCount = BASE_MENU_COUNT + otaAppCount;
  const int bandH = bandHeight(FONT_UI);
  const int pitch = rowPitch(FONT_UI);
  const int ruleY = footerRuleY(renderer, 2);
  int listTop = 85;
  const int needed = menuCount * pitch;
  if (listTop + needed > ruleY - 4) {
    listTop = ruleY - 4 - needed;
    if (listTop < 40) listTop = 40;
  }
  for (int i = 0; i < menuCount; i++) {
    int bandTop = listTop + (i * pitch);
    int textY = textYInBand(FONT_UI, bandTop, bandH);
    const char* label = (i < BASE_MENU_COUNT) ? baseMenuItems[i]
                                              : otaApps[i - BASE_MENU_COUNT].name;
    if (i == mainMenuSelection) {
      clippedFillRect(renderer, 5, bandTop, sw - 10, bandH, tc);
      drawClippedText(renderer, FONT_UI, 20, textY, label, sw - 40, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 20, textY, label, sw - 40, tc);
    }
  }

  // Footer
  if (ruleY > 120) {
    clippedLine(renderer, 10, ruleY, sw - 10, ruleY, tc);
    drawClippedText(renderer, FONT_SMALL, 20, footerLineY(ruleY, 0),
                    "Arrows: Navigate  Enter: Select", 0, tc);
    drawBleStatus(renderer, 20, footerLineY(ruleY, 1));
  }
  drawBattery(renderer, gpio);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Notes", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  int fc = getFileCount();
  const int bandH = bandHeight(FONT_UI);
  const int pitch = rowPitch(FONT_UI);
  int listTop = 42;
  const int ruleY = footerRuleY(renderer, 1);
  int maxVisible = (ruleY - 6 - listTop) / pitch;
  if (maxVisible < 1) maxVisible = 1;
  int startIdx = 0;
  if (fc > maxVisible && selectedFileIndex >= maxVisible) {
    startIdx = selectedFileIndex - maxVisible + 1;
  }

  if (fc == 0) {
    drawClippedText(renderer, FONT_UI, 20, listTop + 14, "No notes yet.", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, listTop + 14 + lineStep(FONT_UI),
                    "Press Ctrl+N to create one.", 0, tc);
  }

  FileInfo* files = getFileList();
  for (int i = startIdx; i < fc && (i - startIdx) < maxVisible; i++) {
    int bandTop = listTop + (i - startIdx) * pitch;
    int textY = textYInBand(FONT_UI, bandTop, bandH);

    if (i == selectedFileIndex) {
      clippedFillRect(renderer, 5, bandTop, sw - 10, bandH, tc);
      drawClippedText(renderer, FONT_UI, 15, textY, files[i].title, sw - 30, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 15, textY, files[i].title, sw - 30, tc);
    }
  }

  // Footer
  clippedLine(renderer, 5, ruleY, sw - 5, ruleY, tc);
  if (deleteConfirmPending && fc > 0) {
    drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0), "Delete? Enter:Yes  Esc:No", 0, tc);
  } else {
    drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0),
                    "Ctrl+N:Title  Ctrl+D:Delete", 0, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Helper: draw a single editor line from the buffer
static void drawEditorLine(GfxRenderer& renderer, int lineIdx, int x, int yPos,
                           int maxW, bool tc) {
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
    drawClippedText(renderer, editorFontId(fontSize), x, yPos, lineBuf, maxW, tc);
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

  int cursorX = 10 + renderer.getTextAdvanceX(editorFontId(fontSize), prefix);
  int cursorW = renderer.getSpaceWidth(editorFontId(fontSize));
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 && cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, tc);
    drawPendingDeadKey(renderer, editorFontId(fontSize), cursorX, cursorW,
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
static int drawEditorHeader(GfxRenderer& renderer, HalGPIO& gpio, int sw, bool tc,
                            const char* centerText = nullptr) {
  if (cleanMode) return 8;

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
  // Mode indicator — fixed position, right-anchored before battery
  const char* modeInd = getModeIndicator();
  int modeW = renderer.getTextAdvanceX(FONT_SMALL, modeInd);
  int modeX = sw - 70 - modeW;
  drawClippedText(renderer, FONT_SMALL, modeX, 5, modeInd, modeW + 5, tc);

  // Word count — drawn to the left of the mode indicator
  int titleMaxW = modeX - 10;
  if (showWordCount) {
    int wc = editorGetWordCount();
    char wcBuf[24];
    if (wc == 1) snprintf(wcBuf, sizeof(wcBuf), "1 word");
    else         snprintf(wcBuf, sizeof(wcBuf), "%d words", wc);
    int wcW = renderer.getTextAdvanceX(FONT_SMALL, wcBuf);
    int wcX = modeX - 8 - wcW;
    if (wcX > 10) {
      drawClippedText(renderer, FONT_SMALL, wcX, 5, wcBuf, wcW + 5, tc);
      titleMaxW = wcX - 10;
    }
  }

  // Title — stops before word count (or mode indicator if word count hidden)
  drawClippedText(renderer, FONT_SMALL, 10, 5, headerBuf, titleMaxW, tc, EpdFontFamily::BOLD);

  // Centered text (e.g. page indicator)
  if (centerText) {
    int ctW = renderer.getTextAdvanceX(FONT_SMALL, centerText);
    drawClippedText(renderer, FONT_SMALL, (sw - ctW) / 2, 5, centerText, ctW + 5, tc);
  }

  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);
  return 38;
}

void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  int lineHeight = renderer.getLineHeight(editorFontId(fontSize));
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
      drawEditorLine(renderer, curLine, 10, centerY, sw - 20, tc);
    }

    // Draw cursor
    drawEditorCursor(renderer, centerY, lineHeight, sw, tc);

    editorSetVisibleLines(1);

    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- PAGINATION MODE ---
  if (writingMode == WritingMode::PAGINATION) {
    // Pre-compute page info for the header
    // Use a temporary linesPerPage estimate (will be exact since header height is fixed)
    int tempTextTop = cleanMode ? 8 : 38;
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
      drawEditorLine(renderer, pageStart + i, 10, yPos, sw - 20, tc);
    }

    // Draw cursor if on this page
    if (curLine >= pageStart && curLine < pageStart + linesPerPage) {
      int cursorY = textAreaTop + ((curLine - pageStart) * lineHeight);
      drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
    }

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
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();

  // Draw visible lines
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int yPos = textAreaTop + (i * lineHeight);
    drawEditorLine(renderer, vpStart + i, 10, yPos, sw - 20, tc);
  }

  // Draw cursor
  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
    drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Edit Title", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  drawClippedText(renderer, FONT_SMALL, 20, 42, "Note title:", 0, tc);
  int boxY = 68, boxH = bandHeight(FONT_UI) + 4;
  int textY = textYInBand(FONT_UI, boxY, boxH);
  renderer.drawRect(15, boxY, sw - 30, boxH, tc);
  drawClippedText(renderer, FONT_UI, 20, textY, renameBuffer, sw - 50, tc);

  // Cursor — thin bar spanning the text's own ink, so it lines up with the
  // letters instead of with drawText()'s ascender-box origin.
  const TextInk titleInk = inkOf(FONT_UI);
  int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, renameBuffer);
  if (cursorX + 2 < sw - 15) {
    if (inputGetPendingDeadKey() != 0) {
      // Armed accent: a filled block carrying the mark, same as in the editor.
      const int blockW = renderer.getSpaceWidth(FONT_UI) > 2
                         ? renderer.getSpaceWidth(FONT_UI) : 8;
      if (cursorX + blockW < sw - 15) {
        renderer.fillRect(cursorX, boxY + 3, blockW, boxH - 6, tc);
        drawPendingDeadKey(renderer, FONT_UI, cursorX, blockW, boxY + 3, boxH - 6, tc);
      }
    } else {
      renderer.fillRect(cursorX, textY + titleInk.top, 2, titleInk.height(), tc);
    }
  }

  // Footer
  const int ruleY = footerRuleY(renderer, 1);
  clippedLine(renderer, 5, ruleY, sw - 5, ruleY, tc);
  drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0), "Enter: Confirm   Esc: Cancel", 0, tc);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Settings", 0, !darkMode, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, !darkMode);

  // Setting items: Orientation, Dark Mode, Writing Mode, Font Size, Bluetooth, Paired Keyboards
  static const char* labels[] = {
    "Orientation", "Dark Mode", "Writing Mode", "Font Size",
    "Keyboard", "Sleep Screen", "Sleep Light", "Bluetooth", "Paired Keyboards"
  };
  const int SETTINGS_COUNT = 9;

  // Row geometry comes from the font: the band has to hold cap-top..descender.
  const int bandH = bandHeight(FONT_UI);
  const int ruleY = footerRuleY(renderer, 2);
  int listTop = 44;
  int pitch = rowPitch(FONT_UI);
  if (listTop + SETTINGS_COUNT * pitch > ruleY - 6) {
    pitch = (ruleY - 6 - listTop) / SETTINGS_COUNT;
    if (pitch < bandH + 1) pitch = bandH + 1;   // never squeeze below the ink
  }

  for (int i = 0; i < SETTINGS_COUNT; i++) {
    int bandTop = listTop + (i * pitch);
    int yPos = textYInBand(FONT_UI, bandTop, bandH);
    bool sel = (i == settingsSelection);

    if (sel) {
      clippedFillRect(renderer, 5, bandTop, sw - 10, bandH, !darkMode);
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, darkMode);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, !darkMode);
    }

    // Value on the right
    char val[32] = "";
    if (i == 0) {
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      strcpy(val, "Portrait"); break;
        case Orientation::LANDSCAPE_CW:  strcpy(val, "Landscape CW"); break;
        case Orientation::PORTRAIT_INV:  strcpy(val, "Inverted"); break;
        case Orientation::LANDSCAPE_CCW: strcpy(val, "Landscape CCW"); break;
      }
    } else if (i == 1) {
      strcpy(val, darkMode ? "Dark" : "Light");
    } else if (i == 2) {
      switch (writingMode) {
        case WritingMode::NORMAL:     strcpy(val, "Normal"); break;
        case WritingMode::TYPEWRITER: strcpy(val, "Typewriter"); break;
        case WritingMode::PAGINATION: strcpy(val, "Pagination"); break;
      }
    } else if (i == 3) {
      switch (fontSize) {
        case FontSize::SMALL:  strcpy(val, "Small"); break;
        case FontSize::MEDIUM: strcpy(val, "Medium"); break;
        default:               strcpy(val, "Large"); break;
      }
    } else if (i == 4) {
      switch (keyboardLayout) {
        case KeyboardLayout::ABNT2:   strcpy(val, "ABNT2");   break;
        case KeyboardLayout::US_INTL: strcpy(val, "US-Intl"); break;
        default:                      strcpy(val, "US");      break;
      }
    } else if (i == 5) {
      switch (sleepScreenMode) {
        case SleepScreenMode::SLIDESHOW: strcpy(val, "Slideshow"); break;
        case SleepScreenMode::SHUFFLE:   strcpy(val, "Shuffle");   break;
        default:                         strcpy(val, "Text");      break;
      }
    } else if (i == 6) {
      switch (sleepBrightness) {
        case SleepBrightness::LIGHT:   strcpy(val, "Light");   break;
        case SleepBrightness::LIGHTER: strcpy(val, "Lighter"); break;
        default:                       strcpy(val, "Normal");  break;
      }
    } else if (i == 8) {
      int kbCount = getPairedKeyboardCount();
      if (kbCount == 0) strcpy(val, "None");
      else if (kbCount == 1) strcpy(val, "1 keyboard");
      else snprintf(val, sizeof(val), "%d keyboards", kbCount);
    }

    if (val[0] != '\0') {
      drawRightText(renderer, FONT_UI, sw - 20, yPos, val, sel ? darkMode : !darkMode);
    }
  }

  // Footer
  clippedLine(renderer, 10, ruleY, sw - 10, ruleY, !darkMode);
  drawClippedText(renderer, FONT_SMALL, 20, footerLineY(ruleY, 0),
                  "Arrows:Navigate  Enter:Change  Esc:Back", 0, !darkMode);

  // Raw readout of the last key. HID usage codes identify a key by POSITION,
  // so this says what the keyboard actually sent, not what its keycap claims —
  // the only way to identify a keyboard's layout without a serial console.
  {
    char keyBuf[72];
    inputDescribeLastKey(keyBuf, sizeof(keyBuf));
    drawClippedText(renderer, FONT_SMALL, 20, footerLineY(ruleY, 1), keyBuf, sw - 40, !darkMode);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  renderer.clearScreen();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Bluetooth Devices", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  // The whole screen is stacked from a running cursor.  The status line, the
  // scan/passkey block and the device list each vary in height, and the fixed
  // y values this used before (45, 60, 72, 104) were closer together than a
  // line of text is tall — so as soon as more than one of them was visible,
  // which is exactly while a scan is running, they printed on top of each
  // other and the whole screen bunched up against the header.
  int y = 38;
  const int smallStep = lineStep(FONT_SMALL);

  // Connection status (left) and the stored pairing (right) share one line.
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "Connected to keyboard"; break;
    case BLEState::SCANNING:     status = "Scanning for devices..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "Not connected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, 10, y, status, sw / 2 - 20, tc);

  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName)) {
    char pairedStr[64];
    snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
    drawClippedText(renderer, FONT_SMALL, sw / 2, y, pairedStr, sw / 2 - 10, tc);
  }
  y += smallStep;

  // Pairing code takes over the block; otherwise show scan progress.
  uint32_t passkey = getCurrentPasskey();
  if (passkey > 0) {
    y += 10;
    drawClippedText(renderer, FONT_UI, 20, y, "PAIRING CODE:", 0, tc, EpdFontFamily::BOLD);
    y += lineStep(FONT_UI);
    char passkeyStr[32];
    snprintf(passkeyStr, sizeof(passkeyStr), "%06lu", passkey);
    drawClippedText(renderer, FONT_BODY, 20, y, passkeyStr, 0, tc, EpdFontFamily::BOLD);
    y += lineStep(FONT_BODY) + 4;
    drawClippedText(renderer, FONT_SMALL, 20, y, "Type this code on your keyboard", 0, tc);
    y += smallStep;
    drawClippedText(renderer, FONT_SMALL, 20, y, "then press Enter", 0, tc);
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
    drawClippedText(renderer, FONT_SMALL, 10, y, scanningStr, sw / 2 - 20, tc);

    char foundStr[32];
    snprintf(foundStr, sizeof(foundStr), "Found: %d", getDiscoveredDeviceCount());
    drawClippedText(renderer, FONT_SMALL, sw / 2, y, foundStr, sw / 2 - 10, tc);
    y += smallStep;
  }

  const int ruleY = footerRuleY(renderer, 2);

  // Device list
  int deviceCount = getDiscoveredDeviceCount();
  if (deviceCount > 0) {
    BleDeviceInfo* devices = getDiscoveredDevices();

    y += 8;
    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "Available devices: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, 10, y, headerStr, 0, tc, EpdFontFamily::BOLD);
    y += smallStep;

    const int bandH = bandHeight(FONT_UI);
    const int pitch = rowPitch(FONT_UI);
    const int listTop = y;

    // How many rows actually fit between here and the footer rule.  The old
    // fixed 10 was independent of both the font and where the list started.
    int maxDevicesToShow = (ruleY - 6 - listTop) / pitch;
    if (maxDevicesToShow < 1) maxDevicesToShow = 1;

    int startIndex = 0;
    if (bluetoothDeviceSelection >= maxDevicesToShow) {
      startIndex = bluetoothDeviceSelection - maxDevicesToShow + 1;
    }
    int devicesToShow = (deviceCount - startIndex < maxDevicesToShow)
                        ? deviceCount - startIndex : maxDevicesToShow;

    for (int i = 0; i < devicesToShow; i++) {
      int deviceIndex = startIndex + i;
      int bandTop = listTop + (i * pitch);
      int textY = textYInBand(FONT_UI, bandTop, bandH);

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

      // Available width: leave room for RSSI on the right (~80px)
      int nameMaxW = sw - 100;

      if (isSelected || isConnected) {
        clippedFillRect(renderer, 5, bandTop, sw - 10, bandH, tc);
        drawClippedText(renderer, FONT_UI, 15, textY, deviceLabel, nameMaxW, !tc);
      } else {
        drawClippedText(renderer, FONT_UI, 15, textY, deviceLabel, nameMaxW, tc);
      }

      // RSSI on the right, centred in the same band as the name
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%ddBm", devices[deviceIndex].rssi);
      drawRightText(renderer, FONT_SMALL, sw - 10,
                    textYInBand(FONT_SMALL, bandTop, bandH), rssiStr,
                    (isSelected || isConnected) ? !tc : tc);
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
        drawClippedText(renderer, FONT_SMALL, 15, navY, navHint, 0, tc);
    }
  } else {
    y += 10;
    drawClippedText(renderer, FONT_UI, 20, y, "No devices found", 0, tc);
    y += lineStep(FONT_UI);
    drawClippedText(renderer, FONT_SMALL, 20, y, "Press Enter to scan for devices", 0, tc);
  }

  // Footer
  clippedLine(renderer, 10, ruleY, sw - 10, ruleY, tc);
  drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0), "Enter:Connect  Right:Scan", 0, tc);
  drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 1), "Left:Disconnect  Esc:Back", 0, tc);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawPairedKeyboardsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Paired Keyboards", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  int count = getPairedKeyboardCount();
  if (count == 0) {
    drawClippedText(renderer, FONT_UI, 20, 60, "No paired keyboards", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 60 + lineStep(FONT_UI),
                    "Go to Bluetooth to scan and connect", 0, tc);
  } else {
    std::string currentAddr = getCurrentDeviceAddress();
    const int bandH = bandHeight(FONT_UI);
    const int pitch = rowPitch(FONT_UI);
    int listTop = 44;

    for (int i = 0; i < count; i++) {
      std::string addr, name; uint8_t addrType;
      getPairedKeyboard(i, addr, name, addrType);

      int bandTop = listTop + (i * pitch);
      int textY = textYInBand(FONT_UI, bandTop, bandH);
      bool sel = (i == pairedKeyboardSelection);
      bool active = (!currentAddr.empty() && currentAddr == addr);

      if (sel) {
        clippedFillRect(renderer, 5, bandTop, sw - 10, bandH, tc);
        drawClippedText(renderer, FONT_UI, 15, textY, name.c_str(), sw - 90, !tc);
      } else {
        drawClippedText(renderer, FONT_UI, 15, textY, name.c_str(), sw - 90, tc);
      }

      const int tagY = textYInBand(FONT_SMALL, bandTop, bandH);
      if (active) {
        drawRightText(renderer, FONT_SMALL, sw - 10, tagY, "active", sel ? !tc : tc);
      } else if (!active && i == getLastUsedKeyboardIndex() && currentAddr.empty()) {
        drawRightText(renderer, FONT_SMALL, sw - 10, tagY, "last", sel ? !tc : tc);
      }
    }
  }

  const int ruleY = footerRuleY(renderer, 2);
  clippedLine(renderer, 10, ruleY, sw - 10, ruleY, tc);
  drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0), "Enter:Connect  D:Forget", 0, tc);
  drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 1), "Left:Disconnect  Esc:Back", 0, tc);

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
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  SyncState state = getSyncState();

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5,
                  state == SyncState::AP_ACTIVE ? "Hotspot" : "Sync",
                  0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  switch (state) {
    case SyncState::SCANNING: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Scanning for networks...", sw - 40, tc);
      break;
    }

    case SyncState::NETWORK_LIST: {
      int nc = getNetworkCount();
      int sel = getSelectedNetwork();

      const int ruleY = footerRuleY(renderer, 1);

      if (nc == 0) {
        const char* st = getSyncStatusText();
        drawClippedText(renderer, FONT_UI, 20, 60, st[0] ? st : "No networks found", sw - 40, tc);
        drawClippedText(renderer, FONT_SMALL, 20, 60 + lineStep(FONT_UI),
                        "Enter: Rescan  Esc: Back", 0, tc);
      } else {
        drawClippedText(renderer, FONT_SMALL, 10, 38, "Select network:", 0, tc);

        const int bandH = bandHeight(FONT_UI);
        const int pitch = rowPitch(FONT_UI);
        int listTop = 38 + lineStep(FONT_SMALL);
        int maxVisible = (ruleY - 6 - listTop) / pitch;
        if (maxVisible < 1) maxVisible = 1;
        int startIdx = 0;
        if (nc > maxVisible && sel >= maxVisible) {
          startIdx = sel - maxVisible + 1;
        }

        for (int i = startIdx; i < nc && (i - startIdx) < maxVisible; i++) {
          int bandTop = listTop + (i - startIdx) * pitch;
          int textY = textYInBand(FONT_UI, bandTop, bandH);
          int barsY = bandTop + (bandH - 13) / 2;
          bool isSel = (i == sel);

          // Build display string: signal indicator + lock + saved + SSID
          char label[48];
          snprintf(label, sizeof(label), "%s%s%s",
                   isNetworkEncrypted(i) ? "* " : "  ",
                   isNetworkSaved(i) ? "+ " : "",
                   getNetworkSSID(i));

          if (isSel) {
            clippedFillRect(renderer, 5, bandTop, sw - 10, bandH, tc);
            drawClippedText(renderer, FONT_UI, 15, textY, label, sw - 50, !tc);
            drawSignalBars(renderer, sw - 30, barsY, getNetworkRSSI(i), !tc);
          } else {
            drawClippedText(renderer, FONT_UI, 15, textY, label, sw - 50, tc);
            drawSignalBars(renderer, sw - 30, barsY, getNetworkRSSI(i), tc);
          }
        }
      }

      // Footer
      clippedLine(renderer, 10, ruleY, sw - 10, ruleY, tc);
      drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0),
                      "*=encrypted +=saved  Enter:Select  Esc:Back", 0, tc);
      break;
    }

    case SyncState::PASSWORD_ENTRY: {
      int sel = getSelectedNetwork();
      char heading[48];
      snprintf(heading, sizeof(heading), "Password for %s", getNetworkSSID(sel));
      drawClippedText(renderer, FONT_SMALL, 20, 42, heading, sw - 40, tc);

      // Password field box — sized so the text's ink fits inside it
      const int boxY = 42 + lineStep(FONT_SMALL);
      const int boxH = bandHeight(FONT_UI) + 4;
      renderer.drawRect(15, boxY, sw - 30, boxH, tc);

      // Show dots for password characters (privacy)
      int pLen = getPasswordLen();
      char dots[64];
      for (int i = 0; i < pLen; i++) dots[i] = '*';
      dots[pLen] = '\0';
      const int dotsY = textYInBand(FONT_UI, boxY, boxH);
      drawClippedText(renderer, FONT_UI, 20, dotsY, dots, sw - 50, tc);

      // Cursor
      const TextInk pwInk = inkOf(FONT_UI);
      int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, dots);
      int cursorW = renderer.getSpaceWidth(FONT_UI);
      if (cursorW < 2) cursorW = 8;
      if (cursorX + cursorW < sw)
        renderer.fillRect(cursorX, dotsY + pwInk.top, cursorW, pwInk.height(), tc);

      drawClippedText(renderer, FONT_SMALL, 20, boxY + boxH + 12,
                      "Enter: Connect   Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::CONNECTING: {
      const char* st = getSyncStatusText();
      drawClippedText(renderer, FONT_UI, 20, 80, st, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 80 + lineStep(FONT_UI), "Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::AP_ACTIVE: {
      // Clone of SYNCING's helper-based layout, not DONE/SAVE_PROMPT:
      // those use fixed y and clip on the bottom (see CLAUDE.md).
      const int ruleY = footerRuleY(renderer, 1);
      const int step = lineStep(FONT_SMALL);
      int y = 42;
      char line[64];
      auto drawLine = [&](const char* text) {
        drawClippedText(renderer, FONT_SMALL, 20, y, text, sw - 40, tc);
        y += step;
      };
      snprintf(line, sizeof(line), "SSID: %s", getApSsid());
      drawLine(line);
      snprintf(line, sizeof(line), "Pass: %s", getApPassword());
      drawLine(line);
      snprintf(line, sizeof(line), "IP: %s", getSyncStatusText());
      drawLine(line);
      snprintf(line, sizeof(line), "Clients: %d", getApStationCount());
      drawLine(line);
      snprintf(line, sizeof(line), "Received: %d", getSyncFilesReceived());
      drawLine(line);

      clippedLine(renderer, 10, ruleY, sw - 10, ruleY, tc);
      drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0),
                      "Esc: Stop", 0, tc);
      break;
    }

    case SyncState::SYNCING: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 42, ip, sw - 40, tc, EpdFontFamily::BOLD);

      int sent    = getSyncFilesSent();
      bool pcConn = isPcConnected();

      // Build the stage list. Static so string pointers remain valid after this scope.
      static char fileStageText[MAX_FILES][16];
      static struct { const char* text; bool done; } stages[MAX_FILES + 3];
      int numStages = 0;

      auto push = [&](const char* text, bool done) {
        if (numStages < MAX_FILES + 3) stages[numStages++] = { text, done };
      };

      push("Connected to WiFi", true);
      push("PC connected",      pcConn);

      if (pcConn) {
        for (int i = 0; i < sent && i < MAX_FILES; i++) {
          snprintf(fileStageText[i], sizeof(fileStageText[i]), "File %d sent", i + 1);
          push(fileStageText[i], true);
        }
        push("Sync complete", false);
      }

      // Display with auto-scroll: keep last [x] + next [-] in view
      const int lineH   = lineStep(FONT_SMALL);
      const int listTop = 42 + lineStep(FONT_SMALL);
      const int ruleY   = footerRuleY(renderer, 1);
      int maxVisible = (ruleY - 6 - listTop) / lineH;
      if (maxVisible < 1) maxVisible = 1;

      int lastDone = 0;
      for (int i = 0; i < numStages; i++) {
        if (stages[i].done) lastDone = i;
      }
      int startIdx = 0;
      if (numStages > maxVisible) {
        startIdx = lastDone - maxVisible + 2;
        if (startIdx < 0) startIdx = 0;
        if (startIdx + maxVisible > numStages) startIdx = numStages - maxVisible;
      }

      for (int i = startIdx; i < numStages && (i - startIdx) < maxVisible; i++) {
        int yPos = listTop + (i - startIdx) * lineH;
        char line[52];
        snprintf(line, sizeof(line), "%s %s",
                 stages[i].done ? "[x]" : "[-]", stages[i].text);
        drawClippedText(renderer, FONT_SMALL, 15, yPos, line, sw - 25, tc);
      }

      // Footer
      clippedLine(renderer, 10, ruleY, sw - 10, ruleY, tc);
      char countStr[32];
      snprintf(countStr, sizeof(countStr), "Sent: %d   Esc: Cancel", sent);
      drawClippedText(renderer, FONT_SMALL, 10, footerLineY(ruleY, 0), countStr, sw - 20, tc);
      break;
    }

    case SyncState::DONE: {
      const char* summary = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Sync Complete", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 85, summary, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 125, "Returning to menu...", 0, tc);
      break;
    }

    case SyncState::CONNECT_FAILED: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Connection failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 80 + lineStep(FONT_UI),
                      "Enter: Retry   Esc: Back", 0, tc);
      break;
    }

    case SyncState::SAVE_PROMPT: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Connected!", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 80, ip, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Save password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }

    case SyncState::FORGET_PROMPT: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Saved password failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Forget saved password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

