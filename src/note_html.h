#pragma once

#include <stddef.h>

// --- The HTML shape of the note index ---------------------------------------
//
// Pure, so it can be exercised on the host: no Arduino, no SD, no WebServer.
// Same split, same reason, as note_json.* and zip_writer.* — the page is
// streamed one row at a time (../SPEC-notes-export.md §8, "stream everything"),
// which needs the per-row formatting as a function, and once it is a function
// it may as well be a tested one.
//
// Two escapes live here, and they are NOT the same escape:
//
//   the displayed title -> HTML text        htmlEscape()
//   the filename        -> a URL path       noteHrefIsVerbatim()
//
// Keeping both here, next to each other, is deliberate. noteUriNameIsSafe()
// already rejects everything dangerous to the CARD, and it is tempting to lean
// on that and skip escaping — but that guard is about paths and this is about
// markup. Coupling one invariant to the other by assumption is how the next
// person relaxes one and breaks the other (SPEC §6). They are checked
// independently, here.

// Longest title this will format. The caller's titles come from
// filenameToTitle() into a MAX_TITLE_LEN (40) buffer, so the real inputs are
// well under it; the bound exists so the escape buffer below is a fixed size
// and the worst case is a number rather than a hope.
static constexpr size_t NOTE_HTML_TITLE_MAX = 64;

// What noteHtmlRow() needs in the worst case, so callers do not guess:
// a 255-byte filename (the FAT limit) plus a 64-byte title of nothing but
// '&' (5 bytes each) plus the fixed markup. test_note_html.cpp asserts it.
static constexpr size_t NOTE_HTML_ROW_MAX = 704;

// Escape `src` into `out` for HTML *text* content. & < > " ' all become
// entities; everything else, UTF-8 included, passes through byte for byte.
//
// Whole or nothing: returns the length written, or 0 when it would not fit —
// and on 0 `out` is left empty rather than holding a prefix. Truncating is not
// an option because a cut inside "&amp;" is not a shorter title, it is broken
// markup that the browser then reads into the rest of the document.
//
// '"' and '\'' use the numeric forms (&#34;, &#39;) rather than &quot;/&apos;:
// five bytes instead of six, which is what keeps the worst-case row under
// NOTE_HTML_ROW_MAX, and &apos; is not in HTML 4 at all.
size_t htmlEscape(char* out, size_t n, const char* src);

// True when every byte of `name` reaches the device unchanged inside a URL
// path, so <a href="/notes/NAME"> actually works.
//
// This is narrow on purpose, and the reason is a loop the device cannot close:
// a browser percent-encodes a space in an href, the server does NOT decode the
// URI (WebServer/Parsing.cpp:104 stores it raw), and noteUriNameIsSafe()
// refuses a literal '%'. So "my note.txt" is not reachable by link at all —
// not because of this page, but because of the download endpoint it links to.
// Rather than emit a link that 400s, the page marks the row (see noteHtmlRow).
//
// The set is the URL "unreserved" characters: A-Z a-z 0-9 and - _ . ~
// Every note the device itself creates is inside it, because titleToFilename()
// emits lowercase ASCII, digits and '_'. Only a file dropped on the card by
// hand can fall out.
bool noteHrefIsVerbatim(const char* name);

// "840 B", "9.7 KB", "13 KB", "1.4 MB" — at most 9 bytes plus the NUL.
void noteHtmlSize(unsigned long bytes, char* out, size_t n);

// One <li> row of the index: the title, linked to GET /notes/<name> when the
// name survives a URL (see noteHrefIsVerbatim), and its size.
//
// A name that does not survive still gets a row, marked as being in the
// archive only. It is NOT dropped: this device has no serial console and a
// note missing from a list with no explanation is the kind of silence that
// costs an hour. Mark, never hide.
//
// Returns the bytes written, or 0 when it would not fit — in which case the
// caller must skip the row rather than send a partial one. With a buffer of
// NOTE_HTML_ROW_MAX that cannot happen for any name FAT can hold.
size_t noteHtmlRow(const char* name, const char* title, unsigned long size,
                   char* out, size_t n);
