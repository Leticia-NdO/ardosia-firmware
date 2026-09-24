// Host-side tests for filename derivation, UTF-8 validation, and the
// 16 KB loadFile guard. Compiles the real file_manager.cpp against an
// in-memory SD stub so a mutation of those functions fails here.
//
// Run with test/run.sh.

#include "../src/utf8_util.h"
#include "../src/file_manager.h"
#include "../src/text_editor.h"
#include "SDCardManager.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

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

// Defined in main.cpp on device; file_manager.cpp declares it extern.
UIState currentState = UIState::MAIN_MENU;
NoteSort noteSort = NoteSort::ALPHA_ASC;

static void resetFs() {
  SdMan.testReset();
  SdMan.mkdir("/notes");
  noteIndexInvalidate();
  noteFilterClear();
  currentState = UIState::MAIN_MENU;
  editorInit();
}

static uint16_t dosDate(int year, int month, int day) {
  return static_cast<uint16_t>(((year - 1980) << 9) | (month << 5) | day);
}

// ---------------------------------------------------------------------------

static void testTitleToFilename() {
  printf("titleToFilename / filenameToTitle\n");
  char out[MAX_FILENAME_LEN];

  titleToFilename("Diário", out, MAX_FILENAME_LEN);
  checkStr(out, "diario.txt", "á folds to a (the Diário/Dirio bug)");

  titleToFilename("Ação, coração!", out, MAX_FILENAME_LEN);
  checkStr(out, "acao_coracao.txt", "punctuation stripped, space to underscore");

  titleToFilename("Dom Casmurro", out, MAX_FILENAME_LEN);
  checkStr(out, "dom_casmurro.txt", "spaces and case");

  titleToFilename("", out, MAX_FILENAME_LEN);
  checkStr(out, "note.txt", "empty title becomes note.txt");

  titleToFilename("   ", out, MAX_FILENAME_LEN);
  checkStr(out, "note.txt", "whitespace-only title becomes note.txt");

  titleToFilename("Hello---World", out, MAX_FILENAME_LEN);
  checkStr(out, "hello_world.txt", "repeated separators collapse");

  // Length cap: maxBase = maxLen - 5. With MAX_FILENAME_LEN=64, 59 chars of
  // base plus ".txt".
  std::string longTitle(80, 'A');
  titleToFilename(longTitle.c_str(), out, MAX_FILENAME_LEN);
  check(strlen(out) == 63, "long title is capped at 59 + .txt");
  check(strcmp(out + 59, ".txt") == 0, "capped name still ends in .txt");

  filenameToTitle("my_note_2.txt", out, MAX_TITLE_LEN);
  checkStr(out, "My Note 2", "filenameToTitle round-trip of underscores");

  filenameToTitle("diario.txt", out, MAX_TITLE_LEN);
  checkStr(out, "Diario", "accent is not restored from the filename");
}

static void testBoundedDecoder() {
  printf("utf8NextCodepointBounded\n");

  // Heap buffer sized exactly to the C string. The vendored decoder reads
  // `bytes` past a truncated 3-byte lead, which is two bytes past the NUL —
  // ASan fires if titleToFilename still uses it.
  char* title = new char[3];
  title[0] = 'x';
  title[1] = static_cast<char>(0xE0);  // 3-byte lead, no continuations
  title[2] = '\0';
  char out[MAX_FILENAME_LEN];
  titleToFilename(title, out, MAX_FILENAME_LEN);
  checkStr(out, "x.txt", "truncated 3-byte sequence at NUL does not over-read");
  delete[] title;

  // Same idea without relying on ASan: padding after the NUL must not leak
  // into the filename.
  char padded[8];
  memset(padded, 'A', sizeof(padded));
  padded[0] = 'x';
  padded[1] = static_cast<char>(0xE0);
  padded[2] = '\0';
  titleToFilename(padded, out, MAX_FILENAME_LEN);
  checkStr(out, "x.txt", "bytes past the NUL are not consumed");

  const unsigned char abc[] = {'a', 'b', 'c'};
  const unsigned char* p = abc;
  check(utf8NextCodepointBounded(&p, abc + 3) == 'a', "ascii walk");
  check(utf8NextCodepointBounded(&p, abc + 3) == 'b', "ascii walk 2");
  check(utf8NextCodepointBounded(&p, abc + 3) == 'c', "ascii walk 3");
  check(utf8NextCodepointBounded(&p, abc + 3) == 0, "end returns 0");
  check(p == abc + 3, "pointer rests at end");

  const unsigned char aAcute[] = {0xC3, 0xA1};  // á
  p = aAcute;
  check(utf8NextCodepointBounded(&p, aAcute + 2) == 0x00E1, "á decodes");
  check(p == aAcute + 2, "pointer advanced by 2");

  const unsigned char trunc[] = {0xC3};
  p = trunc;
  const unsigned char* saved = p;
  check(utf8NextCodepointBounded(&p, trunc + 1) == 0, "truncated 2-byte returns 0");
  check(p == saved, "truncated sequence does not advance");
}

static void testUtf8Validate() {
  printf("utf8Validate\n");

  const char* ok = "ação, coração";
  check(utf8Validate(ok, strlen(ok)), "valid pt-br");
  check(utf8Validate("", 0), "empty is valid");
  check(utf8Validate("ascii", 5), "ascii is valid");

  check(!utf8Validate("\x80", 1), "lone continuation");
  check(!utf8Validate("\xC3" "A", 2), "lead without continuation bytes");
  check(!utf8Validate("\xC3", 1), "truncated sequence");
  check(!utf8Validate("\xC0\x80", 2), "overlong NUL");
  check(!utf8Validate("\xC1\x81", 2), "overlong 'A'");
  check(!utf8Validate("\xED\xA0\x80", 3), "UTF-16 surrogate U+D800");

  check(utf8Validate("\xC3\xA1", 2), "á is valid");
  check(utf8Validate("\xE2\x82\xAC", 3), "euro sign is valid");
  check(!utf8Validate(nullptr, 4), "null pointer with n>0 is invalid");
}

static void testDeriveUnique() {
  printf("deriveUniqueFilename\n");
  char out[MAX_FILENAME_LEN];
  resetFs();

  check(deriveUniqueFilename("Dom Casmurro", out, MAX_FILENAME_LEN),
        "first name succeeds");
  checkStr(out, "dom_casmurro.txt", "no collision keeps the slug");

  SdMan.testAddFile("/notes/dom_casmurro.txt", "x");
  check(deriveUniqueFilename("Dom Casmurro", out, MAX_FILENAME_LEN),
        "collision succeeds with suffix");
  checkStr(out, "dom_casmurro_2.txt", "first collision is _2");

  SdMan.testAddFile("/notes/dom_casmurro_2.txt", "x");
  check(deriveUniqueFilename("Dom Casmurro", out, MAX_FILENAME_LEN),
        "second collision succeeds");
  checkStr(out, "dom_casmurro_3.txt", "next collision is _3");

  // Overflow: a 59-char base is legal for the unsuffixed name, but
  // "%s_%d.txt" with that base does not fit in 64 bytes. The suffix
  // must shorten the base, and the result must still end in .txt.
  std::string longTitle(59, 'a');
  titleToFilename(longTitle.c_str(), out, MAX_FILENAME_LEN);
  check(strlen(out) == 63, "unsuffixed long name is 59+.txt");
  char longPath[320];
  snprintf(longPath, sizeof(longPath), "/notes/%s", out);
  SdMan.testAddFile(longPath, "x");

  check(deriveUniqueFilename(longTitle.c_str(), out, MAX_FILENAME_LEN),
        "long colliding name still derives");
  check(strlen(out) < MAX_FILENAME_LEN, "suffixed name fits the buffer");
  check(strlen(out) >= 6 && strcmp(out + strlen(out) - 4, ".txt") == 0,
        "suffixed long name still ends in .txt");

  // Cap of 99: occupying note.txt and note_2.txt .. note_99.txt must fail
  // rather than return an existing name.
  resetFs();
  SdMan.testAddFile("/notes/note.txt", "x");
  for (int i = 2; i <= 99; i++) {
    char p[64];
    snprintf(p, sizeof(p), "/notes/note_%d.txt", i);
    SdMan.testAddFile(p, "x");
  }
  check(!deriveUniqueFilename("note", out, MAX_FILENAME_LEN),
        "99 suffixes taken is an explicit failure");

  check(!deriveUniqueFilename("x", out, 8),
        "buffer too small to hold a collision name fails");
}

static void testResolveNoteFilename() {
  printf("resolveNoteFilename\n");
  char out[MAX_FILENAME_LEN];
  resetFs();

  check(resolveNoteFilename("Dom Casmurro", false, out, MAX_FILENAME_LEN),
        "first resolve succeeds");
  checkStr(out, "dom_casmurro.txt", "new title uses the slug");

  SdMan.testAddFile("/notes/dom_casmurro.txt", "hello");
  check(resolveNoteFilename("Dom Casmurro", false, out, MAX_FILENAME_LEN),
        "existing small file is reused");
  checkStr(out, "dom_casmurro.txt", "append stays on the slug while under rollover");

  std::string full(NOTE_ROLLOVER_SIZE, 'x');
  SdMan.testAddFile("/notes/dom_casmurro.txt", full);
  check(resolveNoteFilename("Dom Casmurro", false, out, MAX_FILENAME_LEN),
        "full file rolls over");
  checkStr(out, "dom_casmurro_2.txt", "rollover is _2");

  SdMan.testAddFile("/notes/dom_casmurro_2.txt", "more");
  check(resolveNoteFilename("Dom Casmurro", false, out, MAX_FILENAME_LEN),
        "latest in the series is used");
  checkStr(out, "dom_casmurro_2.txt", "append goes to _2 when it has room");

  check(resolveNoteFilename("Dom Casmurro", true, out, MAX_FILENAME_LEN),
        "replace ignores the series");
  checkStr(out, "dom_casmurro.txt", "replace always returns the unsuffixed slug");

  check(noteFilenameIsSafe("dom_casmurro.txt"), "plain slug is safe");
  check(!noteFilenameIsSafe("../x.txt"), "traversal is rejected");
  check(!noteFilenameIsSafe("a/b.txt"), "slash is rejected");
  check(!noteFilenameIsSafe("nope"), "missing .txt is rejected");

  check(noteAppendPrefixNewlines("", 0) == 0, "empty file needs no separator");
  check(noteAppendPrefixNewlines("hi", 2) == 2, "no trailing newline gets two");
  check(noteAppendPrefixNewlines("hi\n", 3) == 1, "one trailing newline gets one");
  check(noteAppendPrefixNewlines("hi\n\n", 4) == 0, "already a blank line");
  check(noteAppendPrefixNewlines("\n", 1) == 1, "file that is just a newline");
}

// Deleting the note that is OPEN is the one sequence in this firmware that can
// undo itself: the autosave fires on a ten-second idle, Esc saves on the way
// out of the editor, and so does the power button — so a buffer still marked
// dirty with the filename still set writes the note straight back onto the card
// seconds after it was deleted. deleteOpenNote() disarms the editor before it
// removes anything, and this is what proves the order.
static void testDeleteOpenNote() {
  printf("deleteOpenNote: the note does not come back\n");
  resetFs();

  SdMan.testAddFile("/notes/keeper.txt", "keep me");
  SdMan.testAddFile("/notes/doomed.txt", "delete me");
  refreshFileList();

  loadFile("doomed.txt");
  check(currentState == UIState::TEXT_EDITOR, "(the note is open)");
  checkStr(editorGetCurrentFile(), "doomed.txt", "(and it is the open file)");

  // Typing into it: this is the state that makes the trap real — an unsaved
  // buffer whose next save would recreate the file.
  editorInsertCodepoint('x');
  check(editorHasUnsavedChanges(), "(with unsaved changes)");

  int selection = 1;
  deleteOpenNote(&selection);

  check(!SdMan.exists("/notes/doomed.txt"), "the file is gone");
  check(SdMan.exists("/notes/keeper.txt"), "and the other note is not");
  check(!editorHasUnsavedChanges(), "the editor has nothing left to save");
  checkStr(editorGetCurrentFile(), "", "and no file to save it to");
  checkStr(editorGetCurrentTitle(), "", "nor a title to rebuild one from");
  check(editorGetLength() == 0, "the buffer is empty");
  check(currentState == UIState::FILE_BROWSER, "and we are back in the list");
  check(selection >= 0 && selection < noteVisibleCount(),
        "the list cursor is clamped to the shorter list");

  // THE assertion. Every path out of the editor ends in one of these two, and
  // neither may put the note back.
  saveCurrentFile(false);
  check(!SdMan.exists("/notes/doomed.txt"), "an explicit save does not recreate it");
  saveCurrentFile(true);
  check(!SdMan.exists("/notes/doomed.txt"), "and neither does one with a refresh");
  check(noteVisibleCount() == 1, "the list holds only the surviving note");

  // A deleted note must not leave a row behind in the card's index either.
  SdMan.testAddFile("/notes/doomed.txt", "a different note, same name");
  noteIndexInvalidate();
  refreshFileList();
  check(noteVisibleCount() == 2, "(a new note with the old name)");

  // And with no note open at all it is a no-op rather than a crash or a delete
  // of something arbitrary: the editor has no filename to hand it.
  editorClear();
  editorSetCurrentFile("");
  const int before = noteVisibleCount();
  deleteOpenNote(&selection);
  refreshFileList();
  check(noteVisibleCount() == before, "with no note open, nothing is deleted");
}

static void testLoadFileGuard() {
  printf("loadFile 16 KB guard\n");
  resetFs();

  SdMan.testAddFile("/notes/ola.txt", "olá");
  loadFile("ola.txt");
  checkStr(editorGetBuffer(), "olá", "small file loads intact");
  check(!editorIsReadOnly(), "a file that fits is writable");
  check(currentState == UIState::TEXT_EDITOR, "load opens the editor");

  // A file at the buffer limit cannot fit the trailing NUL. Opening it
  // writable and saving would make the truncation permanent.
  std::string big(TEXT_BUFFER_SIZE, 'x');
  SdMan.testAddFile("/notes/big.txt", big);
  loadFile("big.txt");
  check(editorIsReadOnly(), "oversized file opens read-only");
  check(editorGetLength() == TEXT_BUFFER_SIZE - 1, "prefix is still readable");
  check(editorGetBuffer()[0] == 'x', "prefix is the start of the file");

  const size_t lenBefore = editorGetLength();
  editorInsertCodepoint('z');
  editorDeleteChar();
  editorDeleteForward();
  check(editorGetLength() == lenBefore, "read-only blocks inserts and deletes");
  check(!editorHasUnsavedChanges(), "read-only does not dirty the buffer");

  saveCurrentFile(false);
  check(SdMan.testContent("/notes/big.txt").size() == TEXT_BUFFER_SIZE,
        "save of a read-only file does not truncate the original");

  // Partial UTF-8 at the end of a file that fits: trim, stay writable.
  std::string tail = "abc";
  tail.push_back(static_cast<char>(0xC3));
  SdMan.testAddFile("/notes/tail.txt", tail);
  loadFile("tail.txt");
  checkStr(editorGetBuffer(), "abc", "truncated UTF-8 tail is dropped");
  check(!editorIsReadOnly(), "a fitting file stays writable after trim");

  // Partial UTF-8 at the cut of an oversized file.
  std::string bigCut(TEXT_BUFFER_SIZE, 'y');
  bigCut[TEXT_BUFFER_SIZE - 2] = static_cast<char>(0xC3);
  SdMan.testAddFile("/notes/bigcut.txt", bigCut);
  loadFile("bigcut.txt");
  check(editorIsReadOnly(), "oversized cut file is read-only");
  const size_t n = editorGetLength();
  check(n > 0 && static_cast<unsigned char>(editorGetBuffer()[n - 1]) != 0xC3,
        "oversized load trims a split sequence at the cut");

  createNewFile();
  check(!editorIsReadOnly(), "new note is writable");
}

static void testSearchAndSort() {
  printf("note search and sort\n");

  check(noteTitleMatches("Diario", "rio"), "fragment matches inside the title");
  check(noteTitleMatches("Diario", "DIA"), "match ignores case");
  check(noteTitleMatches("Coracao", "ção"), "query accent folds the same way as the title");
  check(noteTitleMatches("Dom Casmurro", "dom cas"), "a fragment may include a space");
  check(!noteTitleMatches("Diario", "nota"), "unrelated query does not match");
  check(noteTitleMatches("Diario", ""), "empty query matches everything");
  check(noteTitleMatches("Diario", "\xC2\xB7"), "a mark with no ASCII fold matches everything");

  FileInfo a{};
  FileInfo b{};
  strncpy(a.title, "Alpha", MAX_TITLE_LEN - 1);
  strncpy(b.title, "Beta", MAX_TITLE_LEN - 1);
  strncpy(a.filename, "alpha.txt", MAX_FILENAME_LEN - 1);
  strncpy(b.filename, "beta.txt", MAX_FILENAME_LEN - 1);
  a.modTime = 1;
  b.modTime = 2;
  check(noteCompare(&a, &b, NoteSort::ALPHA_ASC) < 0, "A-Z puts Alpha before Beta");
  check(noteCompare(&a, &b, NoteSort::ALPHA_DESC) > 0, "Z-A puts Beta before Alpha");
  check(noteCompare(&a, &b, NoteSort::NEWEST) > 0, "newest puts the higher number first");
  check(noteCompare(&a, &b, NoteSort::OLDEST) < 0, "oldest puts the lower number first");

  FileInfo tie = a;
  tie.modTime = a.modTime;
  strncpy(tie.title, "Beta", MAX_TITLE_LEN - 1);
  strncpy(tie.filename, "beta.txt", MAX_FILENAME_LEN - 1);
  check(noteCompare(&a, &tie, NoteSort::NEWEST) < 0, "equal creation number falls through to the title");

  NoteSeqSeed seeds[3] = {
      {"Zed", 0},
      {"Mid", dosDate(2024, 6, 2)},
      {"Old", dosDate(2020, 1, 1)},
  };
  // fatKey is (date<<16)|time. Rebuild seeds with the packed key.
  seeds[1].fatKey = (static_cast<uint32_t>(dosDate(2024, 6, 2)) << 16) | 1;
  seeds[2].fatKey = (static_cast<uint32_t>(dosDate(2020, 1, 1)) << 16) | 1;
  uint32_t seq[3] = {};
  const uint32_t next = noteAssignInitialSeq(seeds, 3, seq);
  check(next == 4, "next sequence sits above the numbers just handed out");
  check(seq[2] == 1, "older real date is number 1");
  check(seq[1] == 2, "newer real date follows it");
  check(seq[0] == 3, "undated file comes after every dated one");

  resetFs();
  SdMan.testAddFile("/notes/beta.txt", "b");
  SdMan.testAddFile("/notes/alpha.txt", "a");
  noteSort = NoteSort::ALPHA_ASC;
  refreshFileList();
  check(getFileCount() == 2, "both notes are listed");
  checkStr(getFileList()[0].title, "Alpha", "A-Z lists Alpha first");
  checkStr(getFileList()[1].title, "Beta", "A-Z lists Beta second");
  check(getFileList()[0].modTime < getFileList()[1].modTime, "undated notes are numbered in A-Z order");

  noteSort = NoteSort::NEWEST;
  refreshFileList();
  checkStr(getFileList()[0].title, "Beta", "newest lists the later number first");

  noteFilterPushCodepoint('e');
  noteFilterPushCodepoint('t');
  check(noteVisibleCount() == 1, "fragment hides the other note");
  checkStr(noteVisibleAt(0)->title, "Beta", "et matches Beta");
  noteFilterClear();
  check(noteVisibleCount() == 2, "clearing the filter shows both notes again");
  noteSort = NoteSort::ALPHA_ASC;
}

// ---------------------------------------------------------------------------
// The note-order sidecar, /.ardosia/note_seq.bin
//
// This is the one structure Ardosia writes to the card in its own binary
// format, so the format IS the compatibility surface. v2 appended a word count
// to each record; a v1 file has to keep working, because the sequence numbers
// in it are the only record of creation order and Note Order: Newest/Oldest
// sorts by nothing else.
// ---------------------------------------------------------------------------

static void putLE32(std::string& b, uint32_t v) {
  for (int i = 0; i < 4; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
static void putLE16(std::string& b, uint16_t v) {
  for (int i = 0; i < 2; i++) b.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// Builds a sidecar by hand, exactly as an older firmware would have written it.
static std::string buildSidecar(uint8_t version, uint32_t next,
                                const std::vector<std::pair<std::string, uint32_t>>& entries) {
  std::string b;
  putLE32(b, 0x4E534551);                     // 'NSEQ'
  b.push_back(static_cast<char>(version));
  b.append(3, '\0');                          // pad
  putLE32(b, next);
  putLE16(b, static_cast<uint16_t>(entries.size()));
  putLE16(b, 0);                              // pad2
  for (const auto& e : entries) {
    std::string name = e.first;
    name.resize(MAX_FILENAME_LEN, '\0');
    b.append(name);
    putLE32(b, e.second);
    if (version >= 2) putLE32(b, NOTE_WORDS_UNKNOWN);
  }
  return b;
}

static void testSidecarV1IsStillRead() {
  printf("sidecar: a v1 file keeps its creation order\n");
  resetFs();
  SdMan.testAddFile("/notes/alpha.txt", "one two three");
  SdMan.testAddFile("/notes/beta.txt", "four five");
  // beta was created FIRST according to the old index, so it must sort as older
  // even though its name comes second.
  SdMan.mkdir("/.ardosia");
  SdMan.testAddFile("/.ardosia/note_seq.bin",
                    buildSidecar(1, 9, {{"beta.txt", 3}, {"alpha.txt", 7}}));

  refreshFileList();

  check(noteSeqWordsOf("alpha.txt") == NOTE_WORDS_UNKNOWN,
        "a v1 record has no word count, and says so rather than claiming zero");
  check(noteSeqWordsOf("beta.txt") == NOTE_WORDS_UNKNOWN, "same for the other one");

  noteSort = NoteSort::OLDEST;
  refreshFileList();
  FileInfo* first = noteVisibleAt(0);
  check(first != nullptr && strcmp(first->filename, "beta.txt") == 0,
        "the v1 sequence numbers survived: beta is still the older note");
  noteSort = NoteSort::ALPHA_ASC;
}

static void testSidecarUpgradesToV2() {
  printf("sidecar: v1 upgrades in place, keeping the numbers\n");
  resetFs();
  SdMan.testAddFile("/notes/alpha.txt", "one two three");
  SdMan.mkdir("/.ardosia");
  SdMan.testAddFile("/.ardosia/note_seq.bin", buildSidecar(1, 5, {{"alpha.txt", 4}}));
  refreshFileList();

  noteSeqSetWords("alpha.txt", 3);
  check(SdMan.testContent("/.ardosia/note_seq.bin")[4] == 2,
        "writing a count rewrites the file as v2");

  // Reload from the card and the count is still there, alongside the old order.
  noteIndexInvalidate();
  refreshFileList();
  check(noteSeqWordsOf("alpha.txt") == 3, "the word count survives a reload");

  noteSort = NoteSort::OLDEST;
  refreshFileList();
  FileInfo* f = noteVisibleAt(0);
  check(f != nullptr && f->words == 3, "and reaches the browser through FileInfo");
  noteSort = NoteSort::ALPHA_ASC;
}

// The header's record count, read straight back off the card. This is the
// observable that distinguishes "the index was rebuilt" from "the damaged index
// was believed" — the note list itself is rebuilt from the folder either way,
// so counting visible notes proves nothing.
static int sidecarRecordCount() {
  const std::string b = SdMan.testContent("/.ardosia/note_seq.bin");
  if (b.size() < 16) return -1;
  return static_cast<unsigned char>(b[12]) | (static_cast<unsigned char>(b[13]) << 8);
}
static int sidecarVersion() {
  const std::string b = SdMan.testContent("/.ardosia/note_seq.bin");
  return b.size() < 5 ? -1 : static_cast<unsigned char>(b[4]);
}

// Forces the index to be written back, so its contents can be inspected.
static void flushSidecar(const char* name, uint32_t words) { noteSeqSetWords(name, words); }

static void testSidecarRejectsGarbage() {
  printf("sidecar: a damaged file is discarded, not trusted\n");

  // Every damaged form names two notes that do NOT exist on the card. If the
  // index is correctly discarded, the rebuilt one holds exactly the one real
  // note; if it is believed, the phantoms survive into it.
  const std::vector<std::pair<std::string, uint32_t>> phantoms = {
      {"ghost_one.txt", 3}, {"ghost_two.txt", 4}};

  struct Case { const char* what; std::string body; };
  std::vector<Case> cases;
  cases.push_back({"an empty index", ""});
  cases.push_back({"a header cut off after the magic", "NSEQ"});
  cases.push_back({"a file with the wrong magic", std::string("XXXXX") + std::string(20, '\0')});
  cases.push_back({"a version from the future", buildSidecar(99, 9, phantoms)});
  {
    std::string t = buildSidecar(2, 9, phantoms);
    t.resize(t.size() - 10);              // last record cut short
    cases.push_back({"a truncated v2 index", t});
  }
  {
    std::string t = buildSidecar(1, 9, phantoms);
    t.resize(t.size() - 10);
    cases.push_back({"a truncated v1 index", t});
  }

  for (const Case& c : cases) {
    resetFs();
    SdMan.testAddFile("/notes/alpha.txt", "one two");
    SdMan.mkdir("/.ardosia");
    SdMan.testAddFile("/.ardosia/note_seq.bin", c.body);
    refreshFileList();

    check(noteVisibleCount() == 1, "the note list still builds");
    check(noteSeqWordsOf("ghost_one.txt") == NOTE_WORDS_UNKNOWN,
          "a phantom from a damaged index is not adopted");
    flushSidecar("alpha.txt", 2);
    check(sidecarRecordCount() == 1, c.what);
    check(sidecarVersion() == 2, "and it is rewritten in the current version");
  }
}

static void testFreshNotesHaveNoCountYet() {
  printf("sidecar: a note nobody has saved yet has no count\n");

  // Discovered by a folder scan with no index at all.
  resetFs();
  SdMan.testAddFile("/notes/alpha.txt", "one two three");
  refreshFileList();
  check(noteSeqWordsOf("alpha.txt") == NOTE_WORDS_UNKNOWN,
        "a note found on the card has no count until something counts it");
  FileInfo* f = noteVisibleAt(0);
  check(f != nullptr && f->words == NOTE_WORDS_UNKNOWN,
        "and the browser is told it is unknown, not zero");

  // Discovered one at a time, the path a note created here takes.
  resetFs();
  SdMan.testAddFile("/notes/alpha.txt", "one");
  refreshFileList();
  SdMan.testAddFile("/notes/beta.txt", "two three");
  noteSeqAssignNew("beta.txt");
  check(noteSeqWordsOf("beta.txt") == NOTE_WORDS_UNKNOWN,
        "a newly numbered note has no count either");
}

static void testWordCountDoesNotThrashTheCard() {
  printf("sidecar: unchanged counts do not rewrite the card\n");
  resetFs();
  SdMan.testAddFile("/notes/alpha.txt", "one two three");
  refreshFileList();
  noteSeqSetWords("alpha.txt", 3);

  // Autosave runs every ten idle seconds and calls this each time. Rewriting
  // the whole index when nothing moved is pure wear on the card.
  const int before = SdMan.testOpensForWrite();
  for (int i = 0; i < 20; i++) noteSeqSetWords("alpha.txt", 3);
  check(SdMan.testOpensForWrite() == before, "twenty identical counts write nothing");

  noteSeqSetWords("alpha.txt", 4);
  check(SdMan.testOpensForWrite() > before, "a count that actually changed is written");

  // A name with no slot is a no-op, not a new record.
  const int after = SdMan.testOpensForWrite();
  noteSeqSetWords("ghost.txt", 12);
  check(SdMan.testOpensForWrite() == after, "an unknown note writes nothing");
  check(noteSeqWordsOf("ghost.txt") == NOTE_WORDS_UNKNOWN, "and reports unknown");
}

int main() {
  testSearchAndSort();
  testTitleToFilename();
  testBoundedDecoder();
  testUtf8Validate();
  testDeriveUnique();
  testResolveNoteFilename();
  testLoadFileGuard();
  testDeleteOpenNote();
  testSidecarV1IsStillRead();
  testSidecarUpgradesToV2();
  testSidecarRejectsGarbage();
  testFreshNotesHaveNoCountYet();
  testWordCountDoesNotThrashTheCard();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
