#include "note_json.h"

#include <stdio.h>
#include <string.h>

void jsonAppendStr(char* buf, const size_t bufSize, const char* src) {
  if (!buf || bufSize == 0) return;
  if (!src) return;
  size_t pos = strlen(buf);
  while (*src && pos < bufSize - 2) {
    if (*src == '"' || *src == '\\') buf[pos++] = '\\';
    buf[pos++] = *src++;
  }
  buf[pos] = '\0';
}

bool noteJsonIncludes(const char* name) {
  if (!name) return false;
  if (name[0] == '.') return false;
  const size_t len = strlen(name);
  if (len <= 4) return false;
  return strcmp(name + len - 4, ".txt") == 0;
}

size_t noteJsonEntry(const char* name, const unsigned long size, const bool first,
                     char* out, const size_t n) {
  if (!out || n == 0) return 0;
  out[0] = '\0';
  if (!name) return 0;

  // Build into a local buffer first and only commit if the whole thing fits.
  // Formatting straight into `out` and checking afterwards would leave a
  // partial entry behind on failure, which is the corruption this guards.
  char escaped[2 * 256 + 1] = {0};
  jsonAppendStr(escaped, sizeof(escaped), name);

  char entry[NOTE_JSON_ENTRY_MAX];
  const int written = snprintf(entry, sizeof(entry), "%s{\"name\":\"%s\",\"size\":%lu}",
                              first ? "" : ",", escaped, size);
  if (written < 0) return 0;
  if (static_cast<size_t>(written) >= sizeof(entry)) return 0;  // our own buffer
  if (static_cast<size_t>(written) >= n) return 0;              // the caller's

  memcpy(out, entry, static_cast<size_t>(written) + 1);
  return static_cast<size_t>(written);
}
