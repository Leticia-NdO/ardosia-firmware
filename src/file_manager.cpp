#include "file_manager.h"
#include "text_editor.h"
#include "utf8_util.h"
#include <Arduino.h>
#include <SDCardManager.h>
#include <cstdio>
#include <cstring>

// --- File list ---
static FileInfo fileList[MAX_FILES];
static int fileCount = 0;

// Shared state
extern UIState currentState;

// Convert filename to a readable display title.
// "my_note_2.txt" -> "My Note 2"
void filenameToTitle(const char* filename, char* out, int maxLen) {
  int j = 0;
  bool capitalizeNext = true;
  for (int i = 0; filename[i] != '\0' && filename[i] != '.' && j < maxLen - 1; i++) {
    char c = filename[i];
    if (c == '_') {
      if (j > 0) out[j++] = ' ';
      capitalizeNext = true;
    } else {
      if (capitalizeNext && c >= 'a' && c <= 'z') c -= 32;
      capitalizeNext = false;
      out[j++] = c;
    }
  }
  out[j] = '\0';
  if (j == 0) strncpy(out, "Untitled", maxLen - 1);
}

// Fold an accented Latin-1 letter onto its ASCII base (á -> a, ç -> c).
// Returns 0 when there is no sensible ASCII equivalent.
//
// Needed because the note title round-trips through the filename: loadFile()
// rebuilds the title with filenameToTitle(). Simply dropping the non-ASCII
// bytes — which is what this did before UTF-8 input existed — turned "Diário"
// into "dirio.txt" and then back into the title "Dirio".
//
// NOTE the ceiling of this fix: "Diário" comes back as "Diario". The accent is
// preserved inside the note, but not in its title, because the title is not
// stored anywhere except in the filename. Keeping it would mean changing the
// note file format and the sync protocol.
static char foldToAscii(uint32_t cp) {
  if (cp < 0x80) return static_cast<char>(cp);
  // Fold uppercase Latin-1 to lowercase first (À..Þ -> à..þ, skipping ×).
  if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) cp += 0x20;
  switch (cp) {
    case 0x00E0: case 0x00E1: case 0x00E2:
    case 0x00E3: case 0x00E4: case 0x00E5: return 'a';
    case 0x00E7:                           return 'c';
    case 0x00E8: case 0x00E9:
    case 0x00EA: case 0x00EB:              return 'e';
    case 0x00EC: case 0x00ED:
    case 0x00EE: case 0x00EF:              return 'i';
    case 0x00F1:                           return 'n';
    case 0x00F2: case 0x00F3: case 0x00F4:
    case 0x00F5: case 0x00F6:              return 'o';
    case 0x00F9: case 0x00FA:
    case 0x00FB: case 0x00FC:              return 'u';
    case 0x00FD: case 0x00FF:              return 'y';
    default:                               return 0;
  }
}

// Convert a title to a valid FAT filename (lowercase, accents folded to ASCII,
// spaces->underscores, anything else stripped, ".txt" appended).
void titleToFilename(const char* title, char* out, int maxLen) {
  if (maxLen < 9) {  // "note.txt" + NUL
    if (maxLen > 0) out[0] = '\0';
    return;
  }
  int maxBase = maxLen - 5; // room for ".txt" + null
  int j = 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(title);
  // strlen is safe: it stops at the first NUL. The bounded decoder then
  // refuses to walk a truncated multi-byte sequence at that boundary,
  // which is what utf8NextCodepoint would have over-read.
  const unsigned char* end = p + strlen(title);
  uint32_t cp;
  while ((cp = utf8NextCodepointBounded(&p, end)) != 0 && j < maxBase) {
    char c = foldToAscii(cp);
    if (c == 0) continue;
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else if (c == ' ' || c == '_' || c == '-') {
      if (j > 0 && out[j - 1] != '_') out[j++] = '_';
    }
  }
  while (j > 0 && out[j - 1] == '_') j--;
  if (j == 0) { strncpy(out, "note", maxLen - 1); j = 4; }
  strcpy(out + j, ".txt");
}

// Derive a unique /notes/ filename from a title, handling collisions with _2, _3 suffix.
bool deriveUniqueFilename(const char* title, char* out, int maxLen) {
  // "note_99.txt" is 11 bytes including NUL. A smaller buffer cannot hold
  // a collision name, and snprintf would truncate ".txt" off the end.
  if (out == nullptr || maxLen < 12) return false;

  titleToFilename(title, out, maxLen);

  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", out);
  if (!SdMan.exists(path)) return true;

  // Collision — strip .txt, try _2 .. _99. Reserve room for the longest
  // suffix ("_99.txt" + NUL = 8) *before* snprintf, otherwise a 59-char
  // base (legal for the unsuffixed name) turns ".txt" into ".tx".
  const int suffixRoom = 8;
  char base[MAX_FILENAME_LEN];
  size_t baseLen = strlen(out);
  if (baseLen < 4) return false;
  baseLen -= 4;
  if ((int)baseLen > maxLen - suffixRoom) {
    baseLen = (size_t)(maxLen - suffixRoom);
  }
  memcpy(base, out, baseLen);
  base[baseLen] = '\0';
  while (baseLen > 0 && base[baseLen - 1] == '_') {
    base[--baseLen] = '\0';
  }
  if (baseLen == 0) {
    strncpy(base, "note", sizeof(base) - 1);
  }

  for (int suffix = 2; suffix <= 99; suffix++) {
    const int n = snprintf(out, maxLen, "%s_%d.txt", base, suffix);
    if (n < 0 || n >= maxLen) return false;
    snprintf(path, sizeof(path), "/notes/%s", out);
    if (!SdMan.exists(path)) return true;
  }
  // Every slot taken. Do not return a name that already exists: the
  // caller would write over it.
  return false;
}

void fileManagerSetup() {
  if (!SdMan.begin()) {
    DBG_PRINTLN("SD Card mount failed!");
    return;
  }

  if (!SdMan.exists("/notes")) {
    SdMan.mkdir("/notes");
  }

  DBG_PRINTLN("SD Card initialized");
  refreshFileList();
}

void refreshFileList() {
  fileCount = 0;

  auto root = SdMan.open("/notes");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();
  char name[256];

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || fileCount >= MAX_FILES) {
      file.close();
      if (fileCount >= MAX_FILES) break;
      continue;
    }

    int nameLen = strlen(name);
    if (nameLen > 4 && strcasecmp(name + nameLen - 4, ".txt") == 0) {
      strncpy(fileList[fileCount].filename, name, MAX_FILENAME_LEN - 1);
      fileList[fileCount].filename[MAX_FILENAME_LEN - 1] = '\0';

      filenameToTitle(name, fileList[fileCount].title, MAX_TITLE_LEN);
      fileList[fileCount].modTime = 0;
      fileCount++;
    }
    file.close();
  }
  root.close();
  SdMan.sleep();

  DBG_PRINTF("File listing: %d files found\n", fileCount);
}

int getFileCount() { return fileCount; }
FileInfo* getFileList() { return fileList; }

void loadFile(const char* filename) {
  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename);

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    DBG_PRINTF("Could not open: %s\n", path);
    return;
  }

  // TEXT_BUFFER_SIZE includes the NUL. A file that large cannot fit, and
  // silently truncating would make the next save permanent: the truncated
  // body overwrites the original and then the .bak. Open read-only instead
  // and show a prefix so the note is still readable.
  const bool tooLarge = file.size() >= TEXT_BUFFER_SIZE;

  char* buf = editorGetBuffer();
  int readResult = file.read(buf, TEXT_BUFFER_SIZE - 1);
  size_t bytesRead = (readResult > 0) ? (size_t)readResult : 0;
  buf[bytesRead] = '\0';
  utf8TrimPartialTail(buf);  // a cut mid-"á" must not reach the renderer
  bytesRead = strlen(buf);
  file.close();

  editorSetCurrentFile(filename);
  editorLoadBuffer(bytesRead);
  editorSetReadOnly(tooLarge);

  // Title comes from the filename, not the file content
  char title[MAX_TITLE_LEN];
  filenameToTitle(filename, title, MAX_TITLE_LEN);
  editorSetCurrentTitle(title);
  editorSetUnsavedChanges(false);

  currentState = UIState::TEXT_EDITOR;
  SdMan.sleep();
  DBG_PRINTF("Loaded: %s (%d bytes)%s\n", filename, (int)bytesRead,
             tooLarge ? " [read-only, too large]" : "");
}

void saveCurrentFile(bool refreshList) {
  if (editorIsReadOnly()) {
    DBG_PRINTLN("saveCurrentFile: read-only, skipping");
    return;
  }
  const char* filename = editorGetCurrentFile();
  if (filename[0] == '\0') return;

  char path[320], tmpPath[336], bakPath[336];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);

  // Step 1: Write new content to .tmp
  auto file = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    DBG_PRINTF("saveCurrentFile: could not create tmp: %s\n", tmpPath);
    return;
  }

  size_t toWrite = editorGetLength();
  size_t written = file.write((const uint8_t*)editorGetBuffer(), toWrite);
  file.close();

  // Step 2: Verify bytes written match expected length
  if (written != toWrite) {
    DBG_PRINTF("saveCurrentFile: write mismatch (%d/%d) — aborting\n", (int)written, (int)toWrite);
    SdMan.remove(tmpPath);
    return;
  }

  // Step 3: Rotate original → .bak (original is now safe in .tmp, preserve previous .bak)
  if (SdMan.exists(path)) {
    SdMan.remove(bakPath);          // Remove old .bak (if any)
    SdMan.rename(path, bakPath);    // Original becomes new .bak
  }

  // Step 4: Promote .tmp → original
  SdMan.rename(tmpPath, path);

  editorSetUnsavedChanges(false);
  if (refreshList) refreshFileList();
  SdMan.sleep();
  DBG_PRINTF("Saved: %s\n", filename);
}

void createNewFile() {
  editorClear();
  editorSetCurrentFile("");       // filename derived from title when user confirms
  editorSetCurrentTitle("Untitled");
  editorSetUnsavedChanges(true);
}

// Rename a file on disk to match a new title, updating editor state if needed.
bool updateFileTitle(const char* filename, const char* newTitle) {
  char newFilename[MAX_FILENAME_LEN];
  if (!deriveUniqueFilename(newTitle, newFilename, MAX_FILENAME_LEN)) {
    return false;
  }

  if (strcmp(newFilename, filename) != 0) {
    char oldPath[320], newPath[320];
    snprintf(oldPath, sizeof(oldPath), "/notes/%s", filename);
    snprintf(newPath, sizeof(newPath), "/notes/%s", newFilename);
    SdMan.rename(oldPath, newPath);

    if (strcmp(editorGetCurrentFile(), filename) == 0) {
      editorSetCurrentFile(newFilename);
    }
  }

  refreshFileList();
  SdMan.sleep();
  return true;
}

void deleteFile(const char* filename) {
  char path[320], bakPath[336];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
  SdMan.remove(path);
  SdMan.remove(bakPath);
  refreshFileList();
  SdMan.sleep();
  DBG_PRINTF("Deleted: %s\n", filename);
}

bool noteFilenameIsSafe(const char* filename) {
  if (filename == nullptr || filename[0] == '\0') return false;
  if (strchr(filename, '/') || strchr(filename, '\\')) return false;
  if (strstr(filename, "..")) return false;
  const size_t n = strlen(filename);
  if (n < 5 || strcasecmp(filename + n - 4, ".txt") != 0) return false;
  return true;
}

int countNoteFiles() {
  int n = 0;
  auto root = SdMan.open("/notes");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return 0;
  }
  root.rewindDirectory();
  char name[256];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    const int nameLen = (int)strlen(name);
    if (name[0] != '.' && nameLen > 4 && strcasecmp(name + nameLen - 4, ".txt") == 0) {
      n++;
    }
    file.close();
  }
  root.close();
  return n;
}

bool resolveNoteFilename(const char* title, bool replace, char* out, int maxLen) {
  if (out == nullptr || maxLen < 12) return false;
  titleToFilename(title, out, maxLen);
  if (replace) return true;

  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", out);
  if (!SdMan.exists(path)) return true;

  // Walk the series title.txt, title_2.txt, … and keep the last one that
  // exists. Append there unless it has already rolled.
  const int suffixRoom = 8;
  char base[MAX_FILENAME_LEN];
  size_t baseLen = strlen(out);
  if (baseLen < 4) return false;
  baseLen -= 4;
  if ((int)baseLen > maxLen - suffixRoom) baseLen = (size_t)(maxLen - suffixRoom);
  memcpy(base, out, baseLen);
  base[baseLen] = '\0';
  while (baseLen > 0 && base[baseLen - 1] == '_') base[--baseLen] = '\0';
  if (baseLen == 0) strncpy(base, "note", sizeof(base) - 1);

  char lastName[MAX_FILENAME_LEN];
  strncpy(lastName, out, sizeof(lastName) - 1);
  lastName[sizeof(lastName) - 1] = '\0';
  for (int suffix = 2; suffix <= 99; suffix++) {
    char candidate[MAX_FILENAME_LEN];
    const int n = snprintf(candidate, sizeof(candidate), "%s_%d.txt", base, suffix);
    if (n < 0 || n >= (int)sizeof(candidate)) return false;
    snprintf(path, sizeof(path), "/notes/%s", candidate);
    if (!SdMan.exists(path)) break;
    strncpy(lastName, candidate, sizeof(lastName) - 1);
  }

  snprintf(path, sizeof(path), "/notes/%s", lastName);
  auto f = SdMan.open(path, O_RDONLY);
  const uint32_t sz = f ? f.size() : 0;
  if (f) f.close();

  if (sz >= NOTE_ROLLOVER_SIZE) {
    return deriveUniqueFilename(title, out, maxLen);
  }
  strncpy(out, lastName, maxLen - 1);
  out[maxLen - 1] = '\0';
  return true;
}

int noteAppendPrefixNewlines(const char* existing, size_t len) {
  if (existing == nullptr || len == 0) return 0;
  if (len >= 2 && existing[len - 2] == '\n' && existing[len - 1] == '\n') return 0;
  if (existing[len - 1] == '\n') return 1;
  return 2;
}
