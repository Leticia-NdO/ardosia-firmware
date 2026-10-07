// The streaming ZIP writer (src/zip_writer.cpp).
//
// Two kinds of check here, and the second is the one that earns its keep:
//
//  1. the format and the bookkeeping, asserted in C++;
//  2. a real archive written to /tmp, which run.sh then hands to Python's
//     zipfile module. Asserting my own bytes against my own expectations proves
//     only that I am consistent; having an independent implementation open the
//     file, verify its CRCs and read the contents back proves it is a ZIP.
//
// The design constraint worth remembering: the writer holds NO per-entry state,
// because buffering the central directory is ~64 KB for 1000 notes on a device
// with 320 KB of RAM. The caller walks the folder twice and RECOMPUTES each
// entry's offset. That the two walks agree is the invariant most likely to
// break silently, so it is checked directly.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "zip_writer.h"

static int failures = 0;

static void check(const bool ok, const char* what) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static void checkEq(const long got, const long want, const char* what) {
  if (got == want) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n        got  %ld\n        want %ld\n", what, got, want);
  }
}

// --- a sink that collects into a buffer -------------------------------------

struct Buf {
  std::vector<uint8_t> bytes;
  int calls = 0;
  int failAfter = -1;  // -1 never; otherwise fail once this many calls have run
};

static bool bufSink(const void* data, const size_t len, void* ctx) {
  Buf* b = static_cast<Buf*>(ctx);
  if (b->failAfter >= 0 && b->calls >= b->failAfter) return false;
  b->calls++;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  b->bytes.insert(b->bytes.end(), p, p + len);
  return true;
}

static uint32_t rd32(const std::vector<uint8_t>& v, const size_t at) {
  return static_cast<uint32_t>(v[at]) | (static_cast<uint32_t>(v[at + 1]) << 8) |
         (static_cast<uint32_t>(v[at + 2]) << 16) | (static_cast<uint32_t>(v[at + 3]) << 24);
}

static uint16_t rd16(const std::vector<uint8_t>& v, const size_t at) {
  return static_cast<uint16_t>(v[at] | (v[at + 1] << 8));
}

// One note, as the device would see it.
struct Note {
  std::string name;
  std::string body;
};

static uint32_t crcOf(const std::string& s) {
  return zipCrc32Final(zipCrc32Update(ZIP_CRC_INIT, s.data(), s.size()));
}

// The caller's two walks, exactly as wifi_sync.cpp will do them. Returns the
// offsets the SECOND walk recomputed, so a test can compare them against the
// ones the first walk actually emitted.
static std::vector<uint32_t> writeArchive(const std::vector<Note>& notes, Buf* out,
                                          std::vector<uint32_t>* firstWalkOffsets) {
  ZipWriter w;
  zipInit(&w, bufSink, out);

  for (const Note& n : notes) {
    const uint32_t at = zipLocalHeader(&w, n.name.c_str(),
                                       static_cast<uint32_t>(n.body.size()),
                                       crcOf(n.body), 0, 0);
    if (firstWalkOffsets) firstWalkOffsets->push_back(at);
    zipData(&w, n.body.data(), n.body.size());
  }

  zipStartCentral(&w);

  // The second walk. It does NOT remember the offsets above — it rebuilds them,
  // which is what keeps memory O(1) however many notes there are.
  std::vector<uint32_t> recomputed;
  uint32_t off = 0;
  for (const Note& n : notes) {
    recomputed.push_back(off);
    zipCentralHeader(&w, n.name.c_str(), static_cast<uint32_t>(n.body.size()),
                     crcOf(n.body), 0, 0, off);
    off += zipLocalEntrySize(n.name.size(), static_cast<uint32_t>(n.body.size()));
  }

  zipFinish(&w);
  check(zipOk(&w), "the archive completed without the sink failing");
  return recomputed;
}

// --- CRC-32 ------------------------------------------------------------------

static void testCrc() {
  printf("CRC-32, the one both host and device run\n");

  // Known vectors, so this cannot drift to "whatever my code does".
  // zlib.crc32(b"") == 0, crc32(b"a") == 0xE8B7BE43,
  // crc32(b"123456789") == 0xCBF43926 (the canonical check value).
  checkEq(zipCrc32Final(zipCrc32Update(ZIP_CRC_INIT, "", 0)), 0, "the empty string is 0");
  checkEq((long)(uint32_t)zipCrc32Final(zipCrc32Update(ZIP_CRC_INIT, "a", 1)),
          (long)0xE8B7BE43u, "\"a\" matches zlib");
  checkEq((long)(uint32_t)zipCrc32Final(zipCrc32Update(ZIP_CRC_INIT, "123456789", 9)),
          (long)0xCBF43926u, "the canonical check value");

  // Incremental must equal one-shot: the device feeds it 512 bytes at a time.
  const std::string s = "the quick brown fox jumps over the lazy dog, repeatedly";
  uint32_t inc = ZIP_CRC_INIT;
  for (char c : s) inc = zipCrc32Update(inc, &c, 1);
  check(zipCrc32Final(inc) == crcOf(s), "byte-at-a-time equals all-at-once");

  uint32_t chunked = ZIP_CRC_INIT;
  for (size_t i = 0; i < s.size(); i += 7) {
    chunked = zipCrc32Update(chunked, s.data() + i, std::min<size_t>(7, s.size() - i));
  }
  check(zipCrc32Final(chunked) == crcOf(s), "and so does chunk-at-a-time");

  check(zipCrc32Update(ZIP_CRC_INIT, nullptr, 10) == ZIP_CRC_INIT,
        "a null buffer is not dereferenced");
}

// --- the format --------------------------------------------------------------

static void testEmptyArchive() {
  printf("an empty note folder is still a valid archive\n");

  Buf b;
  ZipWriter w;
  zipInit(&w, bufSink, &b);
  zipFinish(&w);

  check(zipOk(&w), "finishing with no entries succeeds");
  checkEq((long)b.bytes.size(), ZIP_END_RECORD_SIZE, "it is just the end record");
  checkEq((long)rd32(b.bytes, 0), (long)0x06054b50u, "which carries the PK\\5\\6 signature");
  checkEq(rd16(b.bytes, 10), 0, "with zero entries");
  checkEq((long)rd32(b.bytes, 16), 0, "and a central directory at offset 0");
}

static void testHeaderFields() {
  printf("the local header says what it should\n");

  const std::vector<Note> notes = {{"nota.txt", "corpo"}};
  Buf b;
  std::vector<uint32_t> first;
  writeArchive(notes, &b, &first);

  checkEq((long)rd32(b.bytes, 0), (long)0x04034b50u, "PK\\3\\4 at offset 0");
  checkEq(rd16(b.bytes, 8), 0, "the method is STORED (0), never deflate");
  checkEq((long)rd32(b.bytes, 14), (long)crcOf("corpo"), "the CRC is the body's");
  checkEq((long)rd32(b.bytes, 18), 5, "the compressed size equals the real size");
  checkEq((long)rd32(b.bytes, 22), 5, "and so does the uncompressed size");
  checkEq(rd16(b.bytes, 26), 8, "the name length is right");
  checkEq(rd16(b.bytes, 28), 0, "there is no extra field");
  check(std::memcmp(&b.bytes[30], "nota.txt", 8) == 0, "the name follows the header");
  check(std::memcmp(&b.bytes[38], "corpo", 5) == 0, "and the body follows the name");

  // Bit 11 declares the name UTF-8. Without it a reader may decode a note
  // title as code page 437, which is how accents become mojibake.
  check((rd16(b.bytes, 6) & (1u << 11)) != 0, "the UTF-8 name flag is set");
}

static void testTheTwoWalksAgree() {
  printf("the second walk recomputes the offsets the first one used\n");

  // The invariant the O(1) design rests on. If these ever diverge the archive
  // still *looks* fine — the central directory simply points at the wrong
  // bytes, and a reader reports corruption for files that were written
  // correctly. So it is checked directly, over awkward shapes.
  const std::vector<Note> notes = {
      {"a.txt", ""},                        // empty note
      {"b.txt", "x"},                       // one byte
      {"uma_nota_com_nome_longo.txt", std::string(5000, 'y')},
      {"c.txt", std::string(1, '\0')},      // a body that is one NUL byte
      {"d.txt", "fim"},
  };

  Buf b;
  std::vector<uint32_t> first;
  const std::vector<uint32_t> second = writeArchive(notes, &b, &first);

  checkEq((long)second.size(), (long)first.size(), "both walks saw the same entries");
  bool same = first.size() == second.size();
  for (size_t i = 0; same && i < first.size(); i++) {
    if (first[i] != second[i]) {
      failures++;
      printf("  FAIL  entry %zu: first walk %u, second walk %u\n", i, first[i], second[i]);
      same = false;
    }
  }
  check(same, "every offset matches, so the central directory points at real headers");

  // And each claimed offset really does land on a local header signature.
  bool allLand = true;
  for (const uint32_t at : second) {
    if (at + 4 <= b.bytes.size() && rd32(b.bytes, at) == 0x04034b50u) continue;
    allLand = false;
    failures++;
    printf("  FAIL  offset %u does not point at a local header\n", at);
    break;
  }
  check(allLand, "and each one points at a PK\\3\\4 signature in the output");
}

static void testEndRecordAccounting() {
  printf("the end record's arithmetic\n");

  const std::vector<Note> notes = {{"a.txt", "one"}, {"b.txt", "two"}};
  Buf b;
  writeArchive(notes, &b, nullptr);

  const size_t end = b.bytes.size() - ZIP_END_RECORD_SIZE;
  checkEq((long)rd32(b.bytes, end), (long)0x06054b50u, "the end record is last");
  checkEq(rd16(b.bytes, end + 10), 2, "it counts both entries");

  const uint32_t cdSize = rd32(b.bytes, end + 12);
  const uint32_t cdStart = rd32(b.bytes, end + 16);
  checkEq((long)(cdStart + cdSize), (long)end,
          "the central directory ends exactly where the end record begins");
  checkEq((long)rd32(b.bytes, cdStart), (long)0x02014b50u,
          "and it starts with a PK\\1\\2 signature");

  // Two entries, each 46 bytes of header plus a 5-byte name.
  checkEq((long)cdSize, 2 * (ZIP_CENTRAL_HEADER_SIZE + 5), "its size is what it should be");
}

static void testNameRefusal() {
  printf("names the writer will not represent\n");

  // noteUriNameIsSafe() already rejects all of these upstream, but the writer
  // does not get to assume its caller: the two guards exist for different
  // reasons and must not be coupled by assumption.
  check(!zipNameIsUsable(nullptr), "null");
  check(!zipNameIsUsable(""), "empty");
  check(!zipNameIsUsable("/abs.txt"), "a leading slash");
  check(!zipNameIsUsable("dir\\file.txt"), "a backslash, which is a separator to a reader");
  check(!zipNameIsUsable("bell\a.txt"), "a control byte");
  check(!zipNameIsUsable(std::string(256, 'a').c_str()), "longer than 255 bytes");
  check(zipNameIsUsable("diário.txt"), "but UTF-8 is fine — bit 11 says so");
  check(zipNameIsUsable(std::string(255, 'a').c_str()), "and 255 bytes exactly is fine");

  // A refused name poisons the archive rather than being silently skipped: the
  // caller has already committed to a Content-Length-less stream, and a
  // half-written entry is worse than a failed download.
  Buf b;
  ZipWriter w;
  zipInit(&w, bufSink, &b);
  zipLocalHeader(&w, "bad\\name.txt", 1, 0, 0, 0);
  check(!zipOk(&w), "a refused name fails the writer rather than being skipped");
}

static void testSinkFailurePropagates() {
  printf("a sink that fails stops everything\n");

  // On the device the sink is the HTTP client, and it fails when the browser
  // goes away mid-download. That must not be mistaken for a complete archive.
  for (int failAt = 0; failAt < 12; failAt++) {
    Buf b;
    b.failAfter = failAt;
    ZipWriter w;
    zipInit(&w, bufSink, &b);
    zipLocalHeader(&w, "a.txt", 3, 0, 0, 0);
    zipData(&w, "abc", 3);
    zipStartCentral(&w);
    zipCentralHeader(&w, "a.txt", 3, 0, 0, 0, 0);
    zipFinish(&w);
    if (!zipOk(&w)) continue;
    failures++;
    printf("  FAIL  a sink failing at call %d still reported success\n", failAt);
    return;
  }
  check(true, "failing at any of the first twelve writes is reported, not hidden");
}

static void testPhaseOrder() {
  printf("the phases cannot be used out of order\n");

  // Writing a local entry after the central directory has started would put
  // file data inside the directory. Better to fail than to emit that.
  Buf b;
  ZipWriter w;
  zipInit(&w, bufSink, &b);
  zipStartCentral(&w);
  zipLocalHeader(&w, "late.txt", 1, 0, 0, 0);
  check(!zipOk(&w), "a local header after the central directory fails");

  Buf b2;
  ZipWriter w2;
  zipInit(&w2, bufSink, &b2);
  zipCentralHeader(&w2, "early.txt", 1, 0, 0, 0, 0);
  check(!zipOk(&w2), "a central header before zipStartCentral fails");

  Buf b3;
  ZipWriter w3;
  zipInit(&w3, bufSink, &b3);
  zipFinish(&w3);
  zipFinish(&w3);
  check(!zipOk(&w3), "finishing twice fails");
}

static void testSizerMatchesTheOutput() {
  printf("the promised Content-Length equals the bytes written\n");

  // The export sends a real Content-Length, computed from names and sizes
  // before anything is read. It has to be EXACTLY right: one byte over and the
  // browser hangs waiting, one byte under and it truncates the archive. So the
  // prediction is compared against what the writer actually emits, over shapes
  // chosen to be awkward.
  const std::vector<std::vector<Note>> cases = {
      {},                                                   // no notes at all
      {{"a.txt", ""}},                                       // one empty note
      {{"a.txt", "x"}},
      {{"a.txt", ""}, {"b.txt", ""}, {"c.txt", ""}},         // all empty
      {{"nome_bem_mais_longo_que_o_normal.txt", "corpo"}},
      {{"a.txt", std::string(16384, 'q')}},                  // the loadFile ceiling
      {{"diário.txt", "ção"}, {"b.txt", std::string(999, 'z')}, {"c.txt", "fim"}},
  };

  for (size_t i = 0; i < cases.size(); i++) {
    ZipSizer z;
    for (const Note& n : cases[i]) {
      zipSizerAdd(&z, n.name.size(), static_cast<uint32_t>(n.body.size()));
    }
    const uint32_t predicted = zipSizerTotal(&z);

    Buf b;
    writeArchive(cases[i], &b, nullptr);

    if (predicted == b.bytes.size() && z.count == cases[i].size()) continue;
    failures++;
    printf("  FAIL  case %zu: predicted %u, wrote %zu (count %u vs %zu)\n", i,
           predicted, b.bytes.size(), z.count, cases[i].size());
    return;
  }
  check(true, "every shape's predicted length matches the bytes written exactly");

  // The empty case on its own, because it is the one a new device hits.
  ZipSizer empty;
  checkEq((long)zipSizerTotal(&empty), ZIP_END_RECORD_SIZE,
          "no notes predicts just the end record");
  checkEq((long)zipSizerTotal(nullptr), ZIP_END_RECORD_SIZE,
          "and a null sizer does not crash the handler");
}

// --- the artifact an independent tool will check -----------------------------

static void writeArchiveForPython() {
  printf("writing /tmp/ardosia_test.zip for an independent reader\n");

  // Deliberately awkward: an empty note, a note with a quote in its name, an
  // accented name, a note larger than one chunk, and a body with CRLF and a NUL.
  const std::vector<Note> notes = {
      {"vazia.txt", ""},
      {"diário.txt", "acentuação e cedilha: ção\n"},
      {"grande.txt", std::string(20000, 'z')},
      // Explicit length: built from a const char* this would stop at the NUL
      // and the case the comment claims would never be exercised. A note body
      // is bytes, not a C string, and the writer must treat it that way.
      {"linhas.txt", std::string("uma\r\nduas\nzero\0byte", 19)},
      {"última.txt", "fim"},
  };

  Buf b;
  writeArchive(notes, &b, nullptr);

  FILE* f = std::fopen("/tmp/ardosia_test.zip", "wb");
  if (!f) {
    failures++;
    printf("  FAIL  could not write /tmp/ardosia_test.zip\n");
    return;
  }
  const size_t n = std::fwrite(b.bytes.data(), 1, b.bytes.size(), f);
  std::fclose(f);
  check(n == b.bytes.size(), "the archive was written out whole");

  // A manifest the Python side checks against, so it verifies CONTENT and not
  // merely that the file opens.
  FILE* m = std::fopen("/tmp/ardosia_test.zip.manifest", "wb");
  if (!m) return;
  for (const Note& note : notes) {
    std::fprintf(m, "%s\t%zu\t%08X\n", note.name.c_str(), note.body.size(),
                 (unsigned)crcOf(note.body));
  }
  std::fclose(m);
  printf("        %zu bytes, %zu entries\n", b.bytes.size(), notes.size());
}

int main() {
  printf("\n=== zip writer ===\n\n");
  testCrc();
  printf("\n");
  testEmptyArchive();
  printf("\n");
  testHeaderFields();
  printf("\n");
  testTheTwoWalksAgree();
  printf("\n");
  testEndRecordAccounting();
  printf("\n");
  testNameRefusal();
  printf("\n");
  testSinkFailurePropagates();
  printf("\n");
  testPhaseOrder();
  printf("\n");
  testSizerMatchesTheOutput();
  printf("\n");
  writeArchiveForPython();
  printf("\n");

  if (failures) {
    printf("FAILURES: %d\n", failures);
    return 1;
  }
  printf("zip writer: every check passed\n");
  return 0;
}
