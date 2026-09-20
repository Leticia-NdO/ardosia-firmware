#pragma once

// UTF-8 helpers owned by this fork.
//
// lib/Utf8/ is vendored: it decodes (utf8NextCodepoint) and truncates, and is
// deliberately left byte-for-byte so a rebase against upstream stays trivial.
// Everything the pt-br work needs on top of it lives here instead.
//
// The editor's text buffer stays a plain char[] — UTF-8 *is* a byte sequence,
// so the buffer never needed to change.  What had to change is every place
// that assumed 1 byte == 1 character: cursor movement, backspace, word wrap.
// These helpers are how those places step over whole characters.

#include <cstdint>
#include <cstddef>

// Number of bytes in the UTF-8 sequence starting with `lead`.
// Always 1..4 — returns 1 for a continuation or invalid byte, so a caller
// walking a corrupt buffer still makes progress instead of stalling.
int utf8SeqLen(unsigned char lead);

// True if `b` starts a character (i.e. is not a 10xxxxxx continuation byte).
inline bool utf8IsLead(char b) {
  return (static_cast<unsigned char>(b) & 0xC0) != 0x80;
}

// Encode one codepoint into `out` (up to 4 bytes, no terminating NUL).
// Returns the byte count, or 0 if the codepoint cannot be encoded.
int utf8Encode(uint32_t cp, char* out);

// Byte index where the character ending just before `pos` starts.
size_t utf8PrevStart(const char* buf, size_t pos);

// Byte index just past the character that starts at `pos`, clamped to `len`.
size_t utf8NextStart(const char* buf, size_t len, size_t pos);

// Number of characters in buf[from, to).
int utf8CountRange(const char* buf, size_t from, size_t to);

// Byte offset reached by stepping `n` characters forward from `from`,
// never going past `to`.
size_t utf8AdvanceCodepoints(const char* buf, size_t from, size_t to, int n);

// Drop a trailing incomplete sequence from a NUL-terminated string.
// Fixed-size buffers are filled with strncpy() in several places; without this
// a title cut at its length limit can end in half an "á", which the renderer
// then draws as a replacement glyph.
void utf8TrimPartialTail(char* buf);
