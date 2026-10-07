#pragma once

#include <stddef.h>
#include <stdint.h>

// --- A streaming, uncompressed ZIP writer -----------------------------------
//
// For exporting every note as one file the browser can save. "Stored" means no
// compression: notes are small text files, the soft AP is the bottleneck by a
// wide margin, and compressing would mean a dependency this build does not have
// (miniz is not an IDF 4.4 component).
//
// Pure — no Arduino, no SD, no WebServer. Bytes go out through a sink callback,
// so the same code serves an HTTP client on the device and a buffer in
// test/test_zip.cpp.
//
// WHY THE API LOOKS LIKE THIS, which is the whole design:
//
// A ZIP's central directory, at the end, repeats every entry's name, CRC, size
// and offset. Buffering that is the same unbounded-memory trap that
// handleFileList() just had removed — measured at ~13 KB for 200 notes and
// ~64 KB for 1000, on a heap that is also holding the AP, the HTTP server, a
// 16 KB text buffer and NimBLE.
//
// So this writer holds NO per-entry state. The caller walks the note folder
// TWICE inside one request: once emitting local headers and data, once emitting
// central-directory headers. Memory is O(1) regardless of note count. The price
// is reading each note twice for its CRC, which is nothing next to sending it
// over the AP.
//
// The second walk needs each entry's local offset, and recomputes it rather
// than remembering it: offsets accumulate by zipLocalEntrySize(). That the two
// walks agree is the invariant worth testing, and test_zip.cpp does.
//
// The walks are safe to assume identical because the whole export is one
// server->handleClient() call on the single Arduino loop task, so no POST can
// add a note between them.
//
// A ZIP's local header carries the CRC and size BEFORE the data, which is why
// they are parameters: the caller must know them up front. The alternative —
// data descriptors (general-purpose bit 3) — would let us stream blind, and is
// deliberately not used: it is less widely handled, and reading a 16 KB note
// twice is cheaper than the compatibility risk.

// --- CRC-32 (IEEE, as ZIP wants it) -----------------------------------------
//
// Implemented here rather than calling esp_rom_crc32_le() so that host and
// device run the SAME code: two implementations that must agree is a divergence
// waiting to happen, and this one is ten lines. Bitwise, no lookup table —
// ~8 operations a byte against an SD read and a Wi-Fi send.
static constexpr uint32_t ZIP_CRC_INIT = 0xFFFFFFFFu;
uint32_t zipCrc32Update(uint32_t crc, const void* data, size_t len);
uint32_t zipCrc32Final(uint32_t crc);

// Where the bytes go. Return false to abort the archive.
using ZipSink = bool (*)(const void* data, size_t len, void* ctx);

struct ZipWriter {
  ZipSink sink = nullptr;
  void* ctx = nullptr;
  uint32_t offset = 0;    // bytes emitted so far — also the next local offset
  uint32_t count = 0;     // entries written to the central directory
  uint32_t cdStart = 0;   // where the central directory began
  uint32_t cdSize = 0;    // its size once written
  uint8_t phase = 0;      // 0 = local entries, 1 = central directory, 2 = done
  bool failed = false;    // sticky: one bad sink write poisons the archive
};

void zipInit(ZipWriter* w, ZipSink sink, void* ctx);

// How many bytes a local entry occupies: header + name + data. The second walk
// uses this to recompute offsets without remembering them.
uint32_t zipLocalEntrySize(size_t nameLen, uint32_t dataSize);

// --- first walk --------------------------------------------------------------

// Emit one local file header. Returns the offset the entry starts at, which is
// what the central directory will have to point back to.
//
// `dosDate` / `dosTime` are FAT-format date and time, which is what both ZIP
// and SdFat::getModifyDateTime() use, so a caller that has the file's timestamp
// can pass it straight through. Zero is accepted and shows as 1980-01-01.
uint32_t zipLocalHeader(ZipWriter* w, const char* name, uint32_t size, uint32_t crc,
                        uint16_t dosDate, uint16_t dosTime);

// Stream file bytes. The writer counts them, so a caller that writes fewer than
// it declared is caught by zipFinish() rather than producing a quietly broken
// archive.
bool zipData(ZipWriter* w, const void* data, size_t len);

// --- second walk -------------------------------------------------------------

void zipStartCentral(ZipWriter* w);

bool zipCentralHeader(ZipWriter* w, const char* name, uint32_t size, uint32_t crc,
                      uint16_t dosDate, uint16_t dosTime, uint32_t localOffset);

// --- close -------------------------------------------------------------------

// Emit the end-of-central-directory record. After this the archive is complete.
bool zipFinish(ZipWriter* w);

// False once anything has gone wrong. Check before trusting the output.
bool zipOk(const ZipWriter* w);

// The fixed sizes, exported because the tests assert against them rather than
// against magic numbers restated in two places.
static constexpr uint32_t ZIP_LOCAL_HEADER_SIZE = 30;
static constexpr uint32_t ZIP_CENTRAL_HEADER_SIZE = 46;
static constexpr uint32_t ZIP_END_RECORD_SIZE = 22;

// --- the exact size, before a single byte is written ------------------------
//
// A stored archive's length is pure arithmetic over names and sizes — no CRC, no
// file reads. That is what lets the export send a real Content-Length instead
// of chunked encoding, so the browser gets a progress bar and knows when the
// download is done.
//
// It also has to be EXACTLY right. One byte over and the browser hangs waiting
// for data that never comes; one byte under and it truncates the archive. So it
// is an accumulator here rather than arithmetic open-coded in the handler, and
// test_zip.cpp asserts it against the bytes the writer actually emits.
struct ZipSizer {
  uint32_t total = 0;
  uint32_t count = 0;
};

void zipSizerAdd(ZipSizer* z, size_t nameLen, uint32_t dataSize);

// Total including the end-of-central-directory record.
uint32_t zipSizerTotal(const ZipSizer* z);

// A ZIP name may not hold a backslash or a leading slash, and must be UTF-8 when
// bit 11 is set — which this writer sets, because note titles are UTF-8.
// noteUriNameIsSafe() already rejects everything dangerous here, but the writer
// does not get to assume its caller: a name it cannot represent is refused.
bool zipNameIsUsable(const char* name);
