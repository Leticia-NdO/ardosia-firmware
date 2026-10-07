// The JSON shape of the note list (src/note_json.cpp).
//
// Why this exists now: handleFileList() used to build the whole array in a heap
// String and hand it to server->send(), which copies it — ~8 KB for 200 notes,
// ~39 KB for 1000, doubled, on a heap already holding the AP, the HTTP server,
// a 16 KB text buffer and NimBLE. It survived because the only caller was
// sync.py on a developer's card; the browser export page makes it the hot path.
// Streaming needs the per-entry formatting as a function, and once it is a
// function it may as well be a tested one.
//
// The property that carries the weight is NEGATIVE and about truncation: an
// entry that does not fit must be refused whole, never written partially. A
// partial entry does not lose one note — it corrupts the array from there on
// and takes every note after it. That is what finding F5 actually was, and
// until this file the fix for it had no test at all.

#include <cstdio>
#include <cstring>
#include <string>

#include "note_json.h"

static int failures = 0;

static void check(const bool ok, const char* what) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static void checkStr(const char* got, const char* want, const char* what) {
  if (std::strcmp(got, want) == 0) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n        got  %s\n        want %s\n", what, got, want);
  }
}

// --- the escaper, which is the F5 fix and had no test ------------------------

static void testEscaper() {
  printf("jsonAppendStr: the F5 fix, finally covered\n");

  char buf[64];

  buf[0] = '\0';
  jsonAppendStr(buf, sizeof(buf), "plain.txt");
  checkStr(buf, "plain.txt", "an ordinary name passes through");

  // The actual F5: a file dropped on the card by hand holding a quote. Raw
  // concatenation closed the JSON string early and broke every note after it.
  buf[0] = '\0';
  jsonAppendStr(buf, sizeof(buf), "say \"hi\".txt");
  checkStr(buf, "say \\\"hi\\\".txt", "a quote is escaped");

  buf[0] = '\0';
  jsonAppendStr(buf, sizeof(buf), "back\\slash.txt");
  checkStr(buf, "back\\\\slash.txt", "a backslash is escaped");

  // UTF-8 must survive byte for byte: note titles are UTF-8 and the accent
  // bytes are not special to JSON.
  buf[0] = '\0';
  jsonAppendStr(buf, sizeof(buf), "di\xC3\xA1rio.txt");
  checkStr(buf, "di\xC3\xA1rio.txt", "UTF-8 bytes pass through untouched");

  // It appends, which is how the two ble_keyboard.cpp callers use it.
  std::strcpy(buf, "a=");
  jsonAppendStr(buf, sizeof(buf), "b\"c");
  checkStr(buf, "a=b\\\"c", "it appends to what is already there");

  // Degenerate inputs must not write anywhere.
  buf[0] = '\0';
  jsonAppendStr(buf, sizeof(buf), nullptr);
  checkStr(buf, "", "a null source writes nothing");
  jsonAppendStr(nullptr, 10, "x");
  check(true, "a null buffer is not dereferenced");
  jsonAppendStr(buf, 0, "x");
  check(true, "a zero size is not written to");

  // A full buffer truncates rather than overruns — and must never stop with a
  // lone backslash, which would escape the closing quote and break the array.
  char tiny[8];
  tiny[0] = '\0';
  jsonAppendStr(tiny, sizeof(tiny), "\"\"\"\"\"\"\"\"\"\"");
  check(std::strlen(tiny) < sizeof(tiny), "a short buffer truncates, still terminated");
  const size_t n = std::strlen(tiny);
  size_t trailing = 0;
  for (size_t i = n; i > 0 && tiny[i - 1] == '\\'; i--) trailing++;
  check(trailing % 2 == 0, "truncation never leaves a dangling escape");
}

// --- the directory filter ----------------------------------------------------

static void testFilter() {
  printf("noteJsonIncludes: what counts as a note\n");

  check(noteJsonIncludes("dom_casmurro.txt"), "a .txt note is included");
  check(!noteJsonIncludes(".hidden.txt"), "a hidden file is skipped");
  check(!noteJsonIncludes("sleep.bmp"), "a wallpaper is not a note");
  check(!noteJsonIncludes("note_seq.bin"), "the order sidecar is not a note");
  check(!noteJsonIncludes("ui_prefs.json"), "the settings backup is not a note");
  check(!noteJsonIncludes(".txt"), "a bare extension is not a note");
  check(!noteJsonIncludes(""), "an empty name is not a note");
  check(!noteJsonIncludes(nullptr), "a null name is not dereferenced");
  check(noteJsonIncludes("UPPER.TXT") == false,
        "the extension test is case-sensitive, matching what the card holds");

  // Found by mutation: comparing only ".tx" passed everything above, because
  // every rejected name here differed from ".txt" well before the last byte.
  // A name one letter off is the case that discriminates, and it is not
  // far-fetched on a card people drop files onto by hand.
  check(!noteJsonIncludes("archive.txz"), "a near-miss extension is not a note");
  check(!noteJsonIncludes("data.txa"), "nor another one");
  check(!noteJsonIncludes("note.tx"), "nor a truncated one");
}

// --- the entry, and the refusal that matters ---------------------------------

static void testEntry() {
  printf("noteJsonEntry: the shape, and the comma\n");

  char out[NOTE_JSON_ENTRY_MAX];

  size_t len = noteJsonEntry("a.txt", 12, true, out, sizeof(out));
  checkStr(out, "{\"name\":\"a.txt\",\"size\":12}", "the first entry has no comma");
  check(len == std::strlen(out), "the returned length is the written length");

  noteJsonEntry("b.txt", 0, false, out, sizeof(out));
  checkStr(out, ",{\"name\":\"b.txt\",\"size\":0}", "a later entry leads with a comma");

  noteJsonEntry("q\".txt", 7, true, out, sizeof(out));
  checkStr(out, "{\"name\":\"q\\\".txt\",\"size\":7}", "the name is escaped inside the entry");

  noteJsonEntry("big.txt", 4294967295UL, true, out, sizeof(out));
  checkStr(out, "{\"name\":\"big.txt\",\"size\":4294967295}", "a 32-bit size is not truncated");
}

static void testRefusesRatherThanTruncates() {
  printf("an entry that will not fit is refused WHOLE\n");

  // The invariant the streaming caller depends on. A partial entry is not a
  // lost note, it is a corrupt array.
  char out[NOTE_JSON_ENTRY_MAX];

  for (size_t cap = 1; cap < 40; cap++) {
    std::string buf(cap + 8, '\xAA');           // sentinel past the end
    const size_t len = noteJsonEntry("some_note.txt", 123, true, &buf[0], cap);
    const bool fits = len > 0;
    if (fits) {
      if (len >= cap) {
        failures++;
        printf("  FAIL  cap %zu: returned %zu, which does not fit\n", cap, len);
        return;
      }
      if (buf[len] != '\0') {
        failures++;
        printf("  FAIL  cap %zu: not NUL-terminated\n", cap);
        return;
      }
    } else {
      // Refused: out must be an empty string, not a partial entry.
      if (buf[0] != '\0') {
        failures++;
        printf("  FAIL  cap %zu: refused but left \"%s\"\n", cap, buf.c_str());
        return;
      }
    }
    // Nothing past the declared capacity may be touched, ever.
    for (size_t i = cap; i < buf.size(); i++) {
      if (buf[i] == '\xAA') continue;
      failures++;
      printf("  FAIL  cap %zu: wrote past the buffer at +%zu\n", cap, i);
      return;
    }
  }
  check(true, "every capacity from 1 to 39 either fits exactly or refuses cleanly");

  char zero[1];
  check(noteJsonEntry("a.txt", 1, true, zero, 0) == 0, "a zero capacity refuses");
  check(noteJsonEntry(nullptr, 1, true, zero, sizeof(zero)) == 0, "a null name refuses");
  check(noteJsonEntry("a.txt", 1, true, nullptr, 10) == 0, "a null buffer refuses");
}

static void testWorstCaseNameFits() {
  printf("the worst name the card can hold still fits\n");

  // SdFat hands back at most 255 bytes. If every one of them is a quote the
  // escaped form is 510, and the entry has to hold that plus the fixed parts
  // and a 20-digit size. NOTE_JSON_ENTRY_MAX exists so callers do not have to
  // work that out, and this is the check that the number is actually right —
  // if it is wrong, the streaming list silently drops notes.
  std::string worst(255, '"');
  char out[NOTE_JSON_ENTRY_MAX];
  const size_t len = noteJsonEntry(worst.c_str(), 18446744073709551615ULL, false, out,
                                   sizeof(out));
  check(len > 0, "255 quotes and a 20-digit size still fit NOTE_JSON_ENTRY_MAX");
  check(len == std::strlen(out), "and the length agrees");

  // Every length up to the limit, so the margin is swept rather than sampled.
  for (size_t n = 1; n <= 255; n++) {
    std::string name(n, '"');
    if (noteJsonEntry(name.c_str(), 4294967295UL, true, out, sizeof(out)) > 0) continue;
    failures++;
    printf("  FAIL  a %zu-quote name was refused at NOTE_JSON_ENTRY_MAX\n", n);
    return;
  }
  check(true, "and so does every all-quotes name from 1 to 255 bytes");
}

static void testAssembledArrayIsWellFormed() {
  printf("the assembled array, as the streaming caller builds it\n");

  // The caller's loop, reproduced: "[" then entries then "]". What this checks
  // is that the comma bookkeeping cannot produce ",{" at the start or "}," at
  // the end — the two ways a hand-rolled JSON array goes wrong.
  const char* names[] = {".skipme.txt", "one.txt", "notes.bin", "t\"wo.txt", "three.txt"};
  const unsigned long sizes[] = {1, 10, 2, 20, 30};

  std::string json = "[";
  char entry[NOTE_JSON_ENTRY_MAX];
  bool first = true;
  for (int i = 0; i < 5; i++) {
    if (!noteJsonIncludes(names[i])) continue;
    const size_t len = noteJsonEntry(names[i], sizes[i], first, entry, sizeof(entry));
    if (len == 0) continue;
    json += entry;
    first = false;
  }
  json += "]";

  checkStr(json.c_str(),
           "[{\"name\":\"one.txt\",\"size\":10},"
           "{\"name\":\"t\\\"wo.txt\",\"size\":20},"
           "{\"name\":\"three.txt\",\"size\":30}]",
           "skipped files leave no stray commas behind");

  // An empty folder must still be a valid empty array, not "[,]" or "[".
  std::string none = "[";
  bool f = true;
  for (const char* n : {".a.txt", "b.bin"}) {
    if (!noteJsonIncludes(n)) continue;
    noteJsonEntry(n, 0, f, entry, sizeof(entry));
    none += entry;
    f = false;
  }
  none += "]";
  checkStr(none.c_str(), "[]", "a folder with no notes is an empty array");
}

int main() {
  printf("\n=== note list JSON ===\n\n");
  testEscaper();
  printf("\n");
  testFilter();
  printf("\n");
  testEntry();
  printf("\n");
  testRefusesRatherThanTruncates();
  printf("\n");
  testWorstCaseNameFits();
  printf("\n");
  testAssembledArrayIsWellFormed();
  printf("\n");

  if (failures) {
    printf("FAILURES: %d\n", failures);
    return 1;
  }
  printf("note list JSON: every check passed\n");
  return 0;
}
