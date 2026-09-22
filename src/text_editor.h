#pragma once

#include "config.h"
#include <cstdint>

void editorInit();
void editorClear();
void editorLoadBuffer(size_t length);  // After filling buffer externally, set length + reset cursor

// Buffer access
char* editorGetBuffer();
size_t editorGetLength();
int editorGetCursorPosition();

// Editing operations
// Takes a Unicode codepoint: a single char cannot carry "á" (two UTF-8 bytes).
// The buffer itself stays UTF-8 bytes; insert/delete move whole characters.
void editorInsertCodepoint(uint32_t cp);
void editorDeleteChar();     // Backspace
void editorDeleteForward();  // Delete key

// Cursor movement. extend=true keeps an anchor so the range between the
// anchor and the cursor becomes the selection (Shift+arrow). extend=false
// collapses an existing selection: left/right jump to its edge and stop,
// the other keys drop it and then move from the caret.
void editorMoveCursorLeft(bool extend = false);
void editorMoveCursorRight(bool extend = false);
void editorMoveCursorUp(bool extend = false);
void editorMoveCursorDown(bool extend = false);
void editorMoveCursorHome(bool extend = false);
void editorMoveCursorEnd(bool extend = false);

// Selection is empty when the anchor and the cursor coincide. The range is
// a half-open byte interval [lo, hi) on character boundaries.
void editorSelectAll();
bool editorHasSelection();
bool editorGetSelectionRange(int* lo, int* hi);

// Clipboard is internal to the device. Copy works on a read-only note;
// cut and paste do not. Paste that would exceed TEXT_BUFFER_SIZE changes
// nothing. The clipboard survives opening or creating another note.
void editorCopy();
void editorCut();
void editorPaste();

// Line/viewport management
void editorSetCharsPerLine(int cpl);
void editorSetVisibleLines(int n);   // Tell editor how many lines are visible on screen
int editorGetStoredVisibleLines();   // Get the last set visible lines count
void editorRecalculateLines();
int editorGetVisibleLines(int lineHeight, int textAreaHeight);
int editorGetViewportStart();
int editorGetCursorLine();
int editorGetCursorCol();
int editorGetLineCount();
int editorGetLinePosition(int lineIndex);

// File metadata
void editorSetCurrentFile(const char* filename);
void editorSetCurrentTitle(const char* title);
const char* editorGetCurrentFile();
const char* editorGetCurrentTitle();
bool editorHasUnsavedChanges();
void editorSetUnsavedChanges(bool v);

// A file that does not fit TEXT_BUFFER_SIZE is opened read-only so a
// truncated prefix cannot be saved back over the original (and its .bak).
bool editorIsReadOnly();
void editorSetReadOnly(bool v);

int editorGetWordCount();
