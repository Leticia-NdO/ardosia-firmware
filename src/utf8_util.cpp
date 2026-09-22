#include "utf8_util.h"

#include <cstring>

int utf8SeqLen(const unsigned char lead) {
  if (lead < 0x80) return 1;           // 0xxxxxxx
  if ((lead >> 5) == 0x6) return 2;    // 110xxxxx
  if ((lead >> 4) == 0xE) return 3;    // 1110xxxx
  if ((lead >> 3) == 0x1E) return 4;   // 11110xxx
  return 1;                            // continuation or invalid: don't stall
}

int utf8Encode(const uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  // Surrogates are not valid scalar values and must never reach the buffer.
  if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  if (cp <= 0x10FFFF) {
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
  }
  return 0;
}

size_t utf8PrevStart(const char* buf, size_t pos) {
  if (pos == 0) return 0;
  size_t p = pos - 1;
  // Walk back over continuation bytes, but never more than a 4-byte sequence:
  // a corrupt run of 0x80s must not send us to the start of the buffer.
  size_t guard = 0;
  while (p > 0 && !utf8IsLead(buf[p]) && guard < 3) {
    --p;
    ++guard;
  }
  return p;
}

size_t utf8NextStart(const char* buf, size_t len, size_t pos) {
  if (pos >= len) return len;
  size_t next = pos + static_cast<size_t>(utf8SeqLen(static_cast<unsigned char>(buf[pos])));
  return next > len ? len : next;
}

int utf8CountRange(const char* buf, size_t from, size_t to) {
  int n = 0;
  for (size_t i = from; i < to; i++) {
    if (utf8IsLead(buf[i])) n++;
  }
  return n;
}

size_t utf8AdvanceCodepoints(const char* buf, size_t from, size_t to, const int n) {
  size_t p = from;
  for (int i = 0; i < n && p < to; i++) {
    p = utf8NextStart(buf, to, p);
  }
  return p;
}

void utf8TrimPartialTail(char* buf) {
  const size_t len = strlen(buf);
  if (len == 0) return;
  const size_t start = utf8PrevStart(buf, len);
  const size_t need = static_cast<size_t>(utf8SeqLen(static_cast<unsigned char>(buf[start])));
  if (start + need > len) buf[start] = '\0';
}

uint32_t utf8NextCodepointBounded(const unsigned char** p, const unsigned char* end) {
  if (p == nullptr || *p == nullptr || *p >= end) return 0;
  const unsigned char lead = **p;
  if (lead == 0) return 0;

  const int bytes = utf8SeqLen(lead);
  if (*p + bytes > end) return 0;
  for (int i = 0; i < bytes; i++) {
    if ((*p)[i] == 0) return 0;
  }

  uint32_t cp;
  if (bytes == 1) {
    cp = lead;
  } else {
    cp = static_cast<uint32_t>(lead & ((1 << (7 - bytes)) - 1));
    for (int i = 1; i < bytes; i++) {
      cp = (cp << 6) | static_cast<uint32_t>((*p)[i] & 0x3F);
    }
  }
  *p += bytes;
  return cp;
}

bool utf8Validate(const char* s, size_t n) {
  if (n == 0) return true;
  if (s == nullptr) return false;

  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* const end = p + n;
  while (p < end) {
    const unsigned char c = *p;
    if (c == 0) return false;  // embedded NUL
    int bytes;
    uint32_t minCp;
    if (c < 0x80) {
      bytes = 1;
      minCp = 0;
    } else if ((c >> 5) == 0x6) {
      bytes = 2;
      minCp = 0x80;
    } else if ((c >> 4) == 0xE) {
      bytes = 3;
      minCp = 0x800;
    } else if ((c >> 3) == 0x1E) {
      bytes = 4;
      minCp = 0x10000;
    } else {
      return false;  // continuation 10xxxxxx or invalid lead 11111xxx
    }

    if (p + bytes > end) return false;

    uint32_t cp;
    if (bytes == 1) {
      cp = c;
    } else {
      cp = static_cast<uint32_t>(c & ((1 << (7 - bytes)) - 1));
      for (int i = 1; i < bytes; i++) {
        if ((p[i] & 0xC0) != 0x80) return false;
        cp = (cp << 6) | static_cast<uint32_t>(p[i] & 0x3F);
      }
    }
    if (cp < minCp) return false;                       // overlong
    if (cp >= 0xD800 && cp <= 0xDFFF) return false;     // UTF-16 surrogate
    if (cp > 0x10FFFF) return false;
    p += bytes;
  }
  return true;
}
