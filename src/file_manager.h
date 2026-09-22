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

// Title search. Empty query matches everything. Both sides are folded
// (lowercase, accents stripped) so "ação" finds the title "Acao", and a
// fragment matches: "rio" finds "Diario".
bool noteTitleMatches(const char* title, const char* query);

// <0 if a belongs before b. Equal seq/title falls through to the title,
// so the list does not jump around.
int noteCompare(const FileInfo* a, const FileInfo* b, NoteSort mode);

// One-time numbering for notes that have no sequence yet. fatKey 0 means
// "no real date" (the 1980 epoch). Dated files come first, oldest first;
// undated files follow, A–Z. seqOut[i] receives 1..n. Returns the next
// sequence number to hand out (n+1).
struct NoteSeqSeed {
  const char* title;
  uint32_t fatKey;
};
uint32_t noteAssignInitialSeq(const NoteSeqSeed* items, int n, uint32_t* seqOut);

// Live title filter over the already-sorted list. The query stored here is
// the folded text, which is also what the footer shows.
void noteFilterPushCodepoint(uint32_t cp);
void noteFilterBackspace();
void noteFilterClear();
const char* noteFilterText();
int noteVisibleCount();
FileInfo* noteVisibleAt(int index);
void noteClampSelection(int* index);

// Drops the in-memory sequence table so the next refresh reloads the card.
void noteIndexInvalidate();

// Give a just-created note the next creation number. No-op if it already has one.
void noteSeqAssignNew(const char* name);
