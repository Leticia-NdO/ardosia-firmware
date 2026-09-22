#pragma once

#include "config.h"

void fileManagerSetup();
void refreshFileList();
int getFileCount();
FileInfo* getFileList();

void loadFile(const char* filename);
void saveCurrentFile(bool refreshList = true);
void createNewFile();

// Title ↔ filename. Exposed so the host suite can exercise them without
// going through SD. titleToFilename folds accents to ASCII; the inverse
// cannot restore them (see the comment on foldToAscii).
void filenameToTitle(const char* filename, char* out, int maxLen);
void titleToFilename(const char* title, char* out, int maxLen);

// Derive a unique /notes/ filename from a title (_2, _3, … on collision).
// Returns false when every suffix through _99 is taken, or when maxLen is
// too small to hold even "note_2.txt". On false, `out` is unspecified —
// the caller must not use it.
bool deriveUniqueFilename(const char* title, char* out, int maxLen);

// Returns false when the new name could not be derived (99 collisions).
bool updateFileTitle(const char* filename, const char* newTitle);
void deleteFile(const char* filename);

// Pick the /notes/ file for a book title. Appends to the latest file in the
// slug series (title.txt, title_2.txt, …) unless that file is already at
// NOTE_ROLLOVER_SIZE, in which case the next free name is used. `replace`
// always returns the unsuffixed slug. Returns false on the 99-suffix cap.
bool resolveNoteFilename(const char* title, bool replace, char* out, int maxLen);

// True if `filename` is a bare "something.txt" with no slash or "..".
bool noteFilenameIsSafe(const char* filename);

// Count of .txt notes on the card, not capped by MAX_FILES.
int countNoteFiles();

// How many leading '\n' to write before appending so the new block is
// separated from existing content by a blank line. 0 if `len` is 0.
int noteAppendPrefixNewlines(const char* existing, size_t len);
