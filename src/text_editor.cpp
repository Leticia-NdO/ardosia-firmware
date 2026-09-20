#include "text_editor.h"
#include "utf8_util.h"
#include <cstring>
#include <algorithm>

// --- Text buffer ---
static char textBuffer[TEXT_BUFFER_SIZE];
static size_t textLength = 0;
static int cursorPosition = 0;

// --- File metadata ---
static char currentFile[MAX_FILENAME_LEN] = "";
static char currentTitle[MAX_TITLE_LEN] = "Untitled";
static bool unsavedChanges = false;

// --- Line management ---
static int linePositions[MAX_LINES];  // Index into textBuffer for start of each line
static int lineCount = 0;
static int cursorLine = 0;
static int cursorCol = 0;
static int viewportStartLine = 0;
static int charsPerLine = 40;
static int storedVisibleLines = 20;  // Updated by renderer each frame
static bool lineBreaksDirty = true;  // Only recompute line breaks when buffer/charsPerLine changes

// Forward declaration
static void ensureCursorVisible(int visibleLines);

// Recalculate line breaks (word wrap) and cursor position.
// The O(textLength) line break loop only runs when the buffer or charsPerLine changed.
// Cursor line/col is always recomputed (cheap O(cursorLine) with early exit).
void editorRecalculateLines() {
  if (lineBreaksDirty) {
    lineCount = 0;
    linePositions[0] = 0;
    lineCount = 1;

    int col = 0;
    int lastSpace = -1;

    // Walks the buffer one CHARACTER at a time, not one byte: "á" is two bytes
    // and must count as a single column, and a line may only ever break on a
    // character boundary — a linePositions[] entry landing mid-sequence would
    // hand the renderer half a character.
    int i = 0;
    while (i < (int)textLength && lineCount < MAX_LINES) {
      const int next = (int)utf8NextStart(textBuffer, textLength, (size_t)i);

      if (textBuffer[i] == '\n') {
        // Hard line break
        if (lineCount < MAX_LINES) {
          linePositions[lineCount] = i + 1;
          lineCount++;
        }
        col = 0;
        lastSpace = -1;
        i = next;
        continue;
      }

      if (textBuffer[i] == ' ') {
        lastSpace = i;
      }

      col++;
      if (col >= charsPerLine) {
        // Word wrap
        int breakPos;
        if (lastSpace > linePositions[lineCount - 1]) {
          breakPos = lastSpace + 1; // Break after space
        } else {
          breakPos = next;  // Hard break mid-word, on a character boundary
        }

        if (lineCount < MAX_LINES) {
          linePositions[lineCount] = breakPos;
          lineCount++;
        }
        col = utf8CountRange(textBuffer, (size_t)breakPos, (size_t)next);
        lastSpace = -1;
      }
      i = next;
    }
    lineBreaksDirty = false;
  }

  // Compute cursor line and column (always — cheap O(cursorLine) with early exit)
  cursorLine = 0;
  for (int i = 1; i < lineCount; i++) {
    if (cursorPosition >= linePositions[i]) {
      cursorLine = i;
    } else {
      break;
    }
  }
  cursorCol = cursorPosition - linePositions[cursorLine];
}

// Ensure cursor is visible by adjusting viewport
static void ensureCursorVisible(int visibleLines) {
  if (visibleLines <= 0) visibleLines = 20; // fallback

  if (cursorLine < viewportStartLine) {
    viewportStartLine = cursorLine;
  } else if (cursorLine >= viewportStartLine + visibleLines) {
    viewportStartLine = cursorLine - visibleLines + 1;
  }

  if (viewportStartLine < 0) viewportStartLine = 0;
  if (viewportStartLine >= lineCount) viewportStartLine = std::max(0, lineCount - 1);
}

void editorInit() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  currentFile[0] = '\0';
  strncpy(currentTitle, "Untitled", MAX_TITLE_LEN - 1);
  unsavedChanges = false;
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editorRecalculateLines();
}

void editorClear() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  unsavedChanges = false;
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editorRecalculateLines();
}

void editorLoadBuffer(size_t length) {
  textLength = length;
  textBuffer[textLength] = '\0';
  cursorPosition = (int)textLength;  // Start at end
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editorRecalculateLines();
  // Scroll to show cursor
  ensureCursorVisible(storedVisibleLines);
}

char* editorGetBuffer() { return textBuffer; }
size_t editorGetLength() { return textLength; }
int editorGetCursorPosition() { return cursorPosition; }

int editorGetWordCount() {
  int count = 0;
  bool inWord = false;
  for (size_t i = 0; i < textLength; i++) {
    char c = textBuffer[i];
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      inWord = false;
    } else {
      if (!inWord) { count++; inWord = true; }
    }
  }
  return count;
}

// Insert one character. Takes a Unicode codepoint rather than a char because a
// single byte cannot express "á" (0xC3 0xA1) — the buffer stays UTF-8 bytes,
// but a character is inserted and removed as one unit.
void editorInsertCodepoint(uint32_t cp) {
  char enc[4];
  const int n = utf8Encode(cp, enc);
  if (n <= 0) return;
  if (textLength + (size_t)n >= TEXT_BUFFER_SIZE - 1) return;

  // Shift the tail right by the encoded length, then drop the bytes in.
  memmove(textBuffer + cursorPosition + n, textBuffer + cursorPosition,
          textLength - (size_t)cursorPosition);
  memcpy(textBuffer + cursorPosition, enc, (size_t)n);
  cursorPosition += n;
  textLength += (size_t)n;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorDeleteChar() {
  if (cursorPosition <= 0 || textLength == 0) return;

  // Remove the whole character, not one byte: deleting half of "á" would leave
  // a stray 0xC3 that the renderer draws as a replacement glyph and that
  // corrupts the saved file.
  const int start = (int)utf8PrevStart(textBuffer, (size_t)cursorPosition);
  const size_t n = (size_t)(cursorPosition - start);
  memmove(textBuffer + start, textBuffer + cursorPosition,
          textLength - (size_t)cursorPosition);
  cursorPosition = start;
  textLength -= n;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorDeleteForward() {
  if (cursorPosition >= (int)textLength) return;

  const size_t end = utf8NextStart(textBuffer, textLength, (size_t)cursorPosition);
  const size_t n = end - (size_t)cursorPosition;
  memmove(textBuffer + cursorPosition, textBuffer + end, textLength - end);
  textLength -= n;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorLeft() {
  if (cursorPosition > 0) {
    cursorPosition = (int)utf8PrevStart(textBuffer, (size_t)cursorPosition);
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorRight() {
  if (cursorPosition < (int)textLength) {
    cursorPosition = (int)utf8NextStart(textBuffer, textLength, (size_t)cursorPosition);
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorUp() {
  // cursorLine/cursorCol are already valid from the previous operation
  if (cursorLine <= 0) return;

  // cursorCol is a byte offset; the column the user sees is a character count,
  // so translate through characters or an accented line shifts the cursor.
  const int col = utf8CountRange(textBuffer, (size_t)linePositions[cursorLine],
                                 (size_t)cursorPosition);

  int targetLine = cursorLine - 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  // Don't land on the trailing newline
  if (lineEnd > lineStart && textBuffer[lineEnd - 1] == '\n') lineEnd--;

  cursorPosition = (int)utf8AdvanceCodepoints(textBuffer, (size_t)lineStart,
                                              (size_t)lineEnd, col);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorDown() {
  if (cursorLine >= lineCount - 1) return;

  const int col = utf8CountRange(textBuffer, (size_t)linePositions[cursorLine],
                                 (size_t)cursorPosition);

  int targetLine = cursorLine + 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  if (lineEnd > lineStart && textBuffer[lineEnd - 1] == '\n') lineEnd--;

  cursorPosition = (int)utf8AdvanceCodepoints(textBuffer, (size_t)lineStart,
                                              (size_t)lineEnd, col);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorHome() {
  cursorPosition = linePositions[cursorLine];
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorEnd() {
  int lineEnd;
  if (cursorLine + 1 < lineCount) {
    lineEnd = linePositions[cursorLine + 1];
    // Step back over newline if present
    if (lineEnd > 0 && textBuffer[lineEnd - 1] == '\n') lineEnd--;
  } else {
    lineEnd = (int)textLength;
  }
  cursorPosition = lineEnd;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorSetCharsPerLine(int cpl) {
  if (cpl != charsPerLine) {
    charsPerLine = cpl;
    lineBreaksDirty = true;
  }
  editorRecalculateLines();
}

void editorSetVisibleLines(int n) {
  if (n > 0) storedVisibleLines = n;
}

int editorGetStoredVisibleLines() {
  return storedVisibleLines;
}

int editorGetVisibleLines(int lineHeight, int textAreaHeight) {
  if (lineHeight <= 0) return 20;
  return textAreaHeight / lineHeight;
}

int editorGetViewportStart() { return viewportStartLine; }
int editorGetCursorLine() { return cursorLine; }
int editorGetCursorCol() { return cursorCol; }
int editorGetLineCount() { return lineCount; }

int editorGetLinePosition(int lineIndex) {
  if (lineIndex < 0 || lineIndex >= lineCount) return 0;
  return linePositions[lineIndex];
}

void editorSetCurrentFile(const char* filename) {
  strncpy(currentFile, filename, MAX_FILENAME_LEN - 1);
  currentFile[MAX_FILENAME_LEN - 1] = '\0';
}

void editorSetCurrentTitle(const char* title) {
  strncpy(currentTitle, title, MAX_TITLE_LEN - 1);
  currentTitle[MAX_TITLE_LEN - 1] = '\0';
  utf8TrimPartialTail(currentTitle);   // a cut at the limit must not split an "á"
}

const char* editorGetCurrentFile() { return currentFile; }
const char* editorGetCurrentTitle() { return currentTitle; }
bool editorHasUnsavedChanges() { return unsavedChanges; }
void editorSetUnsavedChanges(bool v) { unsavedChanges = v; }
