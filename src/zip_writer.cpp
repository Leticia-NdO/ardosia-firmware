#include "zip_writer.h"

#include <string.h>

namespace {

constexpr uint32_t SIG_LOCAL = 0x04034b50u;    // "PK\3\4"
constexpr uint32_t SIG_CENTRAL = 0x02014b50u;  // "PK\1\2"
constexpr uint32_t SIG_END = 0x06054b50u;      // "PK\5\6"

constexpr uint16_t VERSION_NEEDED = 20;  // 2.0 — stored entries
constexpr uint16_t METHOD_STORED = 0;
// Bit 11: the name is UTF-8. Note titles are UTF-8, and without this a reader is
// entitled to decode them as code page 437.
constexpr uint16_t FLAG_UTF8_NAMES = 1u << 11;

constexpr size_t ZIP_NAME_MAX = 255;

bool put(ZipWriter* w, const void* data, const size_t len) {
  if (w->failed) return false;
  if (!w->sink) { w->failed = true; return false; }
  if (!w->sink(data, len, w->ctx)) { w->failed = true; return false; }
  w->offset += static_cast<uint32_t>(len);
  return true;
}

// Explicit little-endian, never a struct write: the format is little-endian by
// specification, not by whatever the compiler lays out, and writing it by hand
// is what makes the host test meaningful on any machine.
bool put16(ZipWriter* w, const uint16_t v) {
  const uint8_t b[2] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>(v >> 8)};
  return put(w, b, 2);
}

bool put32(ZipWriter* w, const uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v & 0xFF), static_cast<uint8_t>((v >> 8) & 0xFF),
                        static_cast<uint8_t>((v >> 16) & 0xFF),
                        static_cast<uint8_t>((v >> 24) & 0xFF)};
  return put(w, b, 4);
}

}  // namespace

uint32_t zipCrc32Update(uint32_t crc, const void* data, const size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  if (!p) return crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= p[i];
    for (int bit = 0; bit < 8; bit++) {
      // The reflected IEEE polynomial. The mask form avoids a branch.
      crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1u) + 1u));
    }
  }
  return crc;
}

uint32_t zipCrc32Final(const uint32_t crc) { return ~crc; }

void zipInit(ZipWriter* w, const ZipSink sink, void* ctx) {
  if (!w) return;
  *w = ZipWriter{};
  w->sink = sink;
  w->ctx = ctx;
}

bool zipNameIsUsable(const char* name) {
  if (!name) return false;
  const size_t len = strlen(name);
  if (len == 0 || len > ZIP_NAME_MAX) return false;
  if (name[0] == '/') return false;              // absolute paths are not portable
  if (strchr(name, '\\')) return false;          // a separator on the reader's side
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; p++) {
    if (*p < 0x20 || *p == 0x7F) return false;   // control bytes
  }
  return true;
}

uint32_t zipLocalEntrySize(const size_t nameLen, const uint32_t dataSize) {
  return ZIP_LOCAL_HEADER_SIZE + static_cast<uint32_t>(nameLen) + dataSize;
}

void zipSizerAdd(ZipSizer* z, const size_t nameLen, const uint32_t dataSize) {
  if (!z) return;
  // Every entry costs its local record once and its central-directory record
  // once. Forgetting the second is the obvious way to get this wrong, and the
  // test that compares this against the writer's output is what catches it.
  z->total += zipLocalEntrySize(nameLen, dataSize);
  z->total += ZIP_CENTRAL_HEADER_SIZE + static_cast<uint32_t>(nameLen);
  z->count++;
}

uint32_t zipSizerTotal(const ZipSizer* z) {
  if (!z) return ZIP_END_RECORD_SIZE;
  return z->total + ZIP_END_RECORD_SIZE;
}

uint32_t zipLocalHeader(ZipWriter* w, const char* name, const uint32_t size,
                        const uint32_t crc, const uint16_t dosDate,
                        const uint16_t dosTime) {
  if (!w) return 0;
  if (w->phase != 0) { w->failed = true; return 0; }
  if (!zipNameIsUsable(name)) { w->failed = true; return 0; }

  const uint32_t at = w->offset;
  const uint16_t nameLen = static_cast<uint16_t>(strlen(name));

  put32(w, SIG_LOCAL);
  put16(w, VERSION_NEEDED);
  put16(w, FLAG_UTF8_NAMES);
  put16(w, METHOD_STORED);
  put16(w, dosTime);
  put16(w, dosDate);
  put32(w, crc);
  put32(w, size);   // compressed == uncompressed, because stored
  put32(w, size);
  put16(w, nameLen);
  put16(w, 0);      // no extra field
  put(w, name, nameLen);
  return at;
}

bool zipData(ZipWriter* w, const void* data, const size_t len) {
  if (!w) return false;
  if (w->phase != 0) { w->failed = true; return false; }
  return put(w, data, len);
}

void zipStartCentral(ZipWriter* w) {
  if (!w || w->failed) return;
  if (w->phase != 0) { w->failed = true; return; }
  w->phase = 1;
  w->cdStart = w->offset;
}

bool zipCentralHeader(ZipWriter* w, const char* name, const uint32_t size,
                      const uint32_t crc, const uint16_t dosDate,
                      const uint16_t dosTime, const uint32_t localOffset) {
  if (!w) return false;
  if (w->phase != 1) { w->failed = true; return false; }
  if (!zipNameIsUsable(name)) { w->failed = true; return false; }

  const uint16_t nameLen = static_cast<uint16_t>(strlen(name));

  put32(w, SIG_CENTRAL);
  put16(w, VERSION_NEEDED);  // version made by
  put16(w, VERSION_NEEDED);  // version needed
  put16(w, FLAG_UTF8_NAMES);
  put16(w, METHOD_STORED);
  put16(w, dosTime);
  put16(w, dosDate);
  put32(w, crc);
  put32(w, size);
  put32(w, size);
  put16(w, nameLen);
  put16(w, 0);  // extra field
  put16(w, 0);  // comment
  put16(w, 0);  // disk number
  put16(w, 0);  // internal attributes
  put32(w, 0);  // external attributes
  put32(w, localOffset);
  put(w, name, nameLen);

  if (!w->failed) w->count++;
  return !w->failed;
}

bool zipFinish(ZipWriter* w) {
  if (!w) return false;
  // An archive with no entries is legal and useful: it is what an empty note
  // folder should produce, and a reader opens it as an empty archive rather
  // than reporting a corrupt file.
  if (w->phase == 0) zipStartCentral(w);
  if (w->phase != 1) { w->failed = true; return false; }

  w->cdSize = w->offset - w->cdStart;
  w->phase = 2;

  put32(w, SIG_END);
  put16(w, 0);  // this disk
  put16(w, 0);  // disk with the central directory
  put16(w, static_cast<uint16_t>(w->count));
  put16(w, static_cast<uint16_t>(w->count));
  put32(w, w->cdSize);
  put32(w, w->cdStart);
  put16(w, 0);  // archive comment
  return !w->failed;
}

bool zipOk(const ZipWriter* w) { return w && !w->failed; }
