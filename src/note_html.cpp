#include "note_html.h"

#include <stdio.h>
#include <string.h>

size_t htmlEscape(char* out, const size_t n, const char* src) {
  if (!out || n == 0) return 0;
  out[0] = '\0';
  if (!src) return 0;

  size_t pos = 0;
  for (const char* p = src; *p; p++) {
    const char* rep = nullptr;
    switch (*p) {
      case '&':  rep = "&amp;"; break;
      case '<':  rep = "&lt;";  break;
      case '>':  rep = "&gt;";  break;
      case '"':  rep = "&#34;"; break;
      case '\'': rep = "&#39;"; break;
      default:   break;
    }
    const size_t need = rep ? strlen(rep) : 1;
    // Refuse whole rather than leave a prefix: a cut inside "&amp;" is not a
    // shorter title, it is markup the browser reads into the rest of the page.
    if (pos + need >= n) {
      out[0] = '\0';
      return 0;
    }
    if (rep) {
      memcpy(out + pos, rep, need);
    } else {
      out[pos] = *p;
    }
    pos += need;
  }
  out[pos] = '\0';
  return pos;
}

bool noteHrefIsVerbatim(const char* name) {
  if (!name || name[0] == '\0') return false;
  for (const unsigned char* p = (const unsigned char*)name; *p; p++) {
    const unsigned char c = *p;
    if (c >= 'a' && c <= 'z') continue;
    if (c >= 'A' && c <= 'Z') continue;
    if (c >= '0' && c <= '9') continue;
    if (c == '-' || c == '_' || c == '.' || c == '~') continue;
    return false;
  }
  return true;
}

void noteHtmlSize(const unsigned long bytes, char* out, const size_t n) {
  if (!out || n == 0) return;
  if (bytes < 1024UL) {
    snprintf(out, n, "%lu B", bytes);
    return;
  }
  const double kb = (double)bytes / 1024.0;
  if (kb < 1024.0) {
    // One decimal only while it earns its place. "9.7 KB" says something
    // "9 KB" does not; "742.3 KB" is noise on a list someone is scanning.
    if (kb < 10.0) snprintf(out, n, "%.1f KB", kb);
    else           snprintf(out, n, "%.0f KB", kb);
    return;
  }
  snprintf(out, n, "%.1f MB", kb / 1024.0);
}

size_t noteHtmlRow(const char* name, const char* title, const unsigned long size,
                   char* out, const size_t n) {
  if (!out || n == 0) return 0;
  out[0] = '\0';
  if (!name || !title) return 0;
  if (strlen(title) > NOTE_HTML_TITLE_MAX) return 0;

  char esc[5 * NOTE_HTML_TITLE_MAX + 1];
  if (htmlEscape(esc, sizeof(esc), title) == 0 && title[0] != '\0') return 0;

  char sz[16];
  noteHtmlSize(size, sz, sizeof(sz));

  // Built in a local buffer and only committed if the whole thing fits. Writing
  // straight into `out` and checking afterwards leaves a partial row behind on
  // failure, and half an <li> takes the rest of the list with it — the same
  // failure shape noteJsonEntry() guards against in the JSON.
  // The href goes through the escaper too. That is a provable no-op today —
  // noteHrefIsVerbatim() permits only [A-Za-z0-9-_.~], none of which is
  // HTML-special, so the name comes back byte for byte and 256 bytes always
  // holds a 255-byte FAT name. It is here so that widening that whitelist
  // later cannot turn a filename into markup: the two guards stay independent
  // instead of one quietly holding the other up.
  char hname[256];
  const bool linkable =
      noteHrefIsVerbatim(name) && htmlEscape(hname, sizeof(hname), name) != 0;

  char row[NOTE_HTML_ROW_MAX];
  int written;
  if (linkable) {
    written = snprintf(row, sizeof(row),
                       "<li><a href=\"/notes/%s\">%s</a><span class=\"s\">%s</span></li>\n",
                       hname, esc, sz);
  } else {
    // No link, because one would 400 (see noteHrefIsVerbatim). The row stays,
    // and says where the note can still be had.
    written = snprintf(row, sizeof(row),
                       "<li><span class=\"d\">%s</span>"
                       "<span class=\"s\">%s &middot; in the .zip</span></li>\n",
                       esc, sz);
  }
  if (written < 0) return 0;
  if ((size_t)written >= sizeof(row)) return 0;  // our own buffer
  if ((size_t)written >= n) return 0;            // the caller's

  memcpy(out, row, (size_t)written + 1);
  return (size_t)written;
}
