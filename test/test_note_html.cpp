// The HTML shape of the note index (src/note_html.cpp).
//
// The page is the first thing in this feature a stranger on the hotspot sees,
// and it is the first place a note's name becomes MARKUP rather than a path.
// Everything underneath it was built on noteUriNameIsSafe(), which is a guard
// about the SD card: it rejects '/', '\', "..", '%' and control bytes, and it
// happily permits '<', '>', '&' and quotes, because none of those can hurt a
// filesystem. So the index needed its own escaping, and the properties worth
// testing are the two that keep the two guards INDEPENDENT:
//
//   * every byte the href whitelist admits is inert in markup, so the two
//     checks cannot be quietly holding each other up;
//   * a row that will not fit is refused whole, never truncated — half an
//     <li> does not lose one note, it takes the rest of the list with it. Same
//     property, same reason, as noteJsonEntry() in test_note_json.cpp.

#include <cstdio>
#include <cstring>
#include <string>

#include "note_html.h"
#include "web_page.h"

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

// --- the escaper -------------------------------------------------------------

static void testEscape() {
  printf("htmlEscape: a title becomes text, not markup\n");

  char buf[256];

  htmlEscape(buf, sizeof(buf), "My Note 2");
  checkStr(buf, "My Note 2", "an ordinary title passes through");

  htmlEscape(buf, sizeof(buf), "<script>alert(1)</script>");
  checkStr(buf, "&lt;script&gt;alert(1)&lt;/script&gt;", "a tag is defanged");

  htmlEscape(buf, sizeof(buf), "Tom & Jerry");
  checkStr(buf, "Tom &amp; Jerry", "an ampersand is escaped");

  // '&' first, or "<" would become "&amp;lt;" — the classic double-escape bug.
  htmlEscape(buf, sizeof(buf), "&lt;");
  checkStr(buf, "&amp;lt;", "an existing entity is escaped once, not twice");

  htmlEscape(buf, sizeof(buf), "say \"hi\" o'clock");
  checkStr(buf, "say &#34;hi&#34; o&#39;clock",
           "both quote kinds are escaped, numerically");

  htmlEscape(buf, sizeof(buf), "Di\xc3\xa1rio");
  checkStr(buf, "Di\xc3\xa1rio", "UTF-8 passes through byte for byte");

  check(htmlEscape(buf, sizeof(buf), "") == 0 && buf[0] == '\0',
        "the empty string writes nothing and reports nothing");
  check(htmlEscape(buf, sizeof(buf), nullptr) == 0 && buf[0] == '\0',
        "a null source is survivable");
  check(htmlEscape(nullptr, 10, "x") == 0, "a null destination is survivable");
  check(htmlEscape(buf, 0, "x") == 0, "a zero-length destination is survivable");
}

static void testEscapeRefusesRatherThanTruncates() {
  printf("htmlEscape: refuses whole, never a prefix\n");

  // "a&" is 1 + 5 = 6 bytes plus the NUL: 7 is exact, 6 must refuse.
  char buf[8];
  check(htmlEscape(buf, 7, "a&") == 6, "the exact fit is accepted");
  checkStr(buf, "a&amp;", "...and is correct");

  check(htmlEscape(buf, 6, "a&") == 0, "one byte short is refused");
  checkStr(buf, "", "...and leaves nothing behind, not 'a&am'");

  // The dangerous truncation is INSIDE an entity: a trailing "&am" would make
  // the browser swallow whatever the next chunk of the page happens to be.
  char small[5];
  check(htmlEscape(small, sizeof(small), "&") == 0,
        "an entity that does not fit is not written partially");
  checkStr(small, "", "...nothing, not '&am'");
}

// --- the href whitelist ------------------------------------------------------

static void testHref() {
  printf("noteHrefIsVerbatim: what survives a URL path unchanged\n");

  check(noteHrefIsVerbatim("my_note.txt"), "a name the device itself makes");
  check(noteHrefIsVerbatim("my_note_2.txt"), "...with a collision suffix");
  check(noteHrefIsVerbatim("NOTES.TXT"), "uppercase, as a card written on a PC has");
  check(noteHrefIsVerbatim("a-b~c.txt"), "the other unreserved characters");

  // A space is the common one, and it cannot work: the browser sends "%20",
  // the server stores the URI undecoded, and noteUriNameIsSafe() rejects '%'.
  check(!noteHrefIsVerbatim("my note.txt"), "a space cannot round-trip");
  check(!noteHrefIsVerbatim("a&b.txt"), "an ampersand is refused");
  check(!noteHrefIsVerbatim("a#b.txt"), "a fragment marker is refused");
  check(!noteHrefIsVerbatim("a+b.txt"), "a plus is refused");
  check(!noteHrefIsVerbatim("50%.txt"), "a percent is refused");
  check(!noteHrefIsVerbatim("Di\xc3\xa1rio.txt"), "a UTF-8 name is refused");
  check(!noteHrefIsVerbatim(""), "the empty name is refused");
  check(!noteHrefIsVerbatim(nullptr), "a null name is survivable");
}

// THE coupling test. noteHtmlRow() puts the name in an attribute, and it is
// only safe there because the whitelist happens to exclude every byte markup
// cares about. "Happens to" is what this turns into a checked fact: if someone
// widens the whitelist to admit, say, a space or a quote, this fails here
// rather than on a stranger's browser.
static void testHrefSetIsInertInMarkup() {
  printf("the two guards are independent, not propping each other up\n");

  char esc[16];
  bool allInert = true;
  for (int c = 1; c < 256; c++) {
    const char name[2] = {(char)c, '\0'};
    if (!noteHrefIsVerbatim(name)) continue;
    htmlEscape(esc, sizeof(esc), name);
    if (std::strcmp(esc, name) != 0) {
      allInert = false;
      printf("        byte 0x%02x is allowed in an href but escapes to %s\n", c, esc);
    }
  }
  check(allInert, "every byte the href whitelist admits is inert in HTML");
}

// --- sizes -------------------------------------------------------------------

static void testSize() {
  printf("noteHtmlSize: readable at a glance\n");

  char buf[16];
  noteHtmlSize(0, buf, sizeof(buf));      checkStr(buf, "0 B", "an empty note");
  noteHtmlSize(1, buf, sizeof(buf));      checkStr(buf, "1 B", "one byte");
  noteHtmlSize(1023, buf, sizeof(buf));   checkStr(buf, "1023 B", "just under 1 KB");
  noteHtmlSize(1024, buf, sizeof(buf));   checkStr(buf, "1.0 KB", "exactly 1 KB");
  noteHtmlSize(2560, buf, sizeof(buf));   checkStr(buf, "2.5 KB", "a decimal while it earns it");
  noteHtmlSize(10240, buf, sizeof(buf));  checkStr(buf, "10 KB", "and no decimal past 10 KB");
  noteHtmlSize(16384, buf, sizeof(buf));  checkStr(buf, "16 KB", "a full text buffer");
  noteHtmlSize(1048576, buf, sizeof(buf)); checkStr(buf, "1.0 MB", "a megabyte");

  // Nine bytes plus the NUL is the documented worst case; the row buffer's
  // arithmetic depends on it.
  noteHtmlSize(1073741823, buf, sizeof(buf));
  check(std::strlen(buf) <= 9, "the longest output still fits in nine bytes");
}

// --- the row -----------------------------------------------------------------

static void testRow() {
  printf("noteHtmlRow: one note, one line\n");

  char row[NOTE_HTML_ROW_MAX];

  noteHtmlRow("my_note.txt", "My Note", 2560, row, sizeof(row));
  checkStr(row,
           "<li><a href=\"/notes/my_note.txt\">My Note</a>"
           "<span class=\"s\">2.5 KB</span></li>\n",
           "a linked row, verbatim");

  // The title comes from filenameToTitle(), so a hostile filename produces a
  // hostile title. Escaped, not trusted.
  noteHtmlRow("ab.txt", "A<b>B", 100, row, sizeof(row));
  check(std::strstr(row, "A&lt;b&gt;B") != nullptr, "the title is escaped in the row");
  check(std::strstr(row, "<b>") == nullptr, "...and the raw tag is nowhere in it");

  // A name a URL cannot carry: no link, but the note is still named, because a
  // note silently missing from the list is worse than one marked unreachable.
  noteHtmlRow("my note.txt", "My Note", 100, row, sizeof(row));
  check(std::strstr(row, "href") == nullptr, "an unlinkable name gets no href");
  check(std::strstr(row, "My Note") != nullptr, "...but still appears by title");
  check(std::strstr(row, "in the .zip") != nullptr, "...and says where to get it");

  check(noteHtmlRow(nullptr, "T", 1, row, sizeof(row)) == 0, "a null name is refused");
  check(noteHtmlRow("a.txt", nullptr, 1, row, sizeof(row)) == 0, "a null title is refused");
  check(noteHtmlRow("a.txt", "T", 1, nullptr, 10) == 0, "a null destination is survivable");
}

static void testRowRefusesRatherThanTruncates() {
  printf("noteHtmlRow: a row that will not fit is skipped, not cut\n");

  char tiny[32];
  check(noteHtmlRow("my_note.txt", "My Note", 2560, tiny, sizeof(tiny)) == 0,
        "a destination too small is refused");
  checkStr(tiny, "", "...and left empty, so the caller cannot send half an <li>");

  // Over the documented title bound: refused rather than silently shortened,
  // so nobody builds a layout on a truncation this does not promise.
  std::string longTitle(NOTE_HTML_TITLE_MAX + 1, 'x');
  char row[NOTE_HTML_ROW_MAX];
  check(noteHtmlRow("a.txt", longTitle.c_str(), 1, row, sizeof(row)) == 0,
        "a title past NOTE_HTML_TITLE_MAX is refused");
}

// The number NOTE_HTML_ROW_MAX promises. Worst case the card can actually
// produce: a 255-byte FAT name (all of it href-legal, so it is not shortened
// by the escape) and a title of nothing but '&', which is the five-byte
// entity. If this ever stops fitting, the constant is wrong and rows start
// disappearing from the page for no visible reason.
static void testWorstCaseRowFits() {
  printf("the worst case the card can hold still fits\n");

  const std::string name(251, 'a');
  const std::string title(NOTE_HTML_TITLE_MAX, '&');

  char row[NOTE_HTML_ROW_MAX];
  const size_t len =
      noteHtmlRow((name + ".txt").c_str(), title.c_str(), 1073741823, row, sizeof(row));
  check(len > 0, "a 255-byte name with a 64-byte all-entity title is formatted");
  check(len == std::strlen(row), "...and the reported length is the real one");
  printf("        worst case: %zu bytes of %zu\n", len, (size_t)NOTE_HTML_ROW_MAX);
}

// The page the handler actually streams: head, rows, tail. Nothing here checks
// the chrome in src/web_page.h (it is a literal, and a test that retypes a
// literal only proves it can retype it) — what it checks is that the pieces
// the DEVICE assembles nest properly.
static void testAssembledListIsWellFormed() {
  printf("the assembled list nests\n");

  struct { const char* name; const char* title; unsigned long size; } notes[] = {
      {"first.txt", "First", 120},
      {"a&b.txt", "A&B", 2048},            // unlinkable AND needing escapes
      {"third_note_2.txt", "Third Note 2", 40000},
  };

  std::string page = "<ul>\n";
  char row[NOTE_HTML_ROW_MAX];
  for (const auto& nt : notes) {
    const size_t len = noteHtmlRow(nt.name, nt.title, nt.size, row, sizeof(row));
    check(len > 0, "every row in an ordinary folder is formatted");
    page += row;
  }
  page += "</ul>\n";

  int open = 0, close = 0;
  for (size_t i = 0; i + 3 < page.size(); i++) {
    if (page.compare(i, 4, "<li>") == 0) open++;
    if (page.compare(i, 5, "</li>") == 0) close++;
  }
  check(open == 3 && close == 3, "one opened and one closed <li> per note");

  // No bare '&' survives anywhere: every one of them must begin an entity this
  // module is known to emit. Checked against a closed list rather than "looks
  // like an entity", because "&amp" with the semicolon lost would pass the
  // loose version. This is the check that would catch the escaper being
  // skipped on either the title or the href.
  static const char* kEntities[] = {"&amp;", "&lt;", "&gt;", "&#34;", "&#39;",
                                    "&middot;"};
  bool bare = false;
  for (size_t i = 0; i < page.size(); i++) {
    if (page[i] != '&') continue;
    bool known = false;
    for (const char* e : kEntities) {
      if (page.compare(i, std::strlen(e), e) == 0) known = true;
    }
    if (!known) {
      bare = true;
      printf("        bare '&' at offset %zu: %s\n", i, page.substr(i, 12).c_str());
    }
  }
  check(!bare, "every ampersand on the page begins a known entity");
}

// --- the whole page, for an independent parser to read -----------------------
//
// The chrome in src/web_page.h is a literal, and a C++ test that retypes a
// literal only proves it can retype it. What is worth checking is that the
// pieces NEST — an unclosed tag inside that <style> block would show up as a
// blank page on someone's phone and nowhere else, because this device has no
// way to serve a page to a validator.
//
// So the three branches the handler can produce are written out here and
// check-page.py reads them back with Python's HTML parser. Same arrangement,
// same reason, as test_zip.cpp and check-zip.py: my own bytes against my own
// expectations prove only that I am self-consistent.
//
// This mirrors handleIndex() in wifi_sync.cpp by hand, and can drift from it.
// The sequence is kept deliberately short for that reason — the order of the
// literals is the only thing duplicated.
static void writePages() {
  printf("writing the three pages for check-page.py\n");

  struct { const char* name; const char* title; unsigned long size; } notes[] = {
      {"first_note.txt", "First Note", 120},
      {"diario.txt", "Diario", 2048},
      {"notes_2.txt", "Notes 2", 40000},
      {"a&b.txt", "A&B", 900},          // hand-dropped: escaped, and no link
      {"my note.txt", "My Note", 70},   // hand-dropped: no URL can carry it
  };

  std::string full = WEB_PAGE_HEAD;
  char head[80];
  snprintf(head, sizeof(head), WEB_PAGE_COUNT_FMT, 5, "s");
  full += head;
  full += WEB_PAGE_BUTTON;
  full += WEB_PAGE_LIST_OPEN;
  char row[NOTE_HTML_ROW_MAX];
  for (const auto& nt : notes) {
    if (noteHtmlRow(nt.name, nt.title, nt.size, row, sizeof(row)) > 0) full += row;
  }
  full += WEB_PAGE_LIST_CLOSE;
  full += WEB_PAGE_TAIL;

  std::string empty = WEB_PAGE_HEAD;
  snprintf(head, sizeof(head), WEB_PAGE_COUNT_FMT, 0, "s");
  empty += head;
  empty += WEB_PAGE_EMPTY;
  empty += WEB_PAGE_TAIL;

  std::string noCard = WEB_PAGE_HEAD;
  snprintf(head, sizeof(head), WEB_PAGE_COUNT_FMT, 0, "s");
  noCard += head;
  noCard += WEB_PAGE_NO_CARD;
  noCard += WEB_PAGE_TAIL;

  struct { const char* path; const std::string* body; } out[] = {
      {"/tmp/ardosia_page_full.html", &full},
      {"/tmp/ardosia_page_empty.html", &empty},
      {"/tmp/ardosia_page_nocard.html", &noCard},
  };
  for (const auto& o : out) {
    FILE* f = std::fopen(o.path, "wb");
    if (!f) {
      failures++;
      printf("  FAIL  could not write %s\n", o.path);
      continue;
    }
    std::fwrite(o.body->data(), 1, o.body->size(), f);
    std::fclose(f);
    printf("  ok    %s (%zu bytes)\n", o.path, o.body->size());
  }

  // The payload the firmware actually carries, so it can be compared against
  // the ~8 KB threshold in the spec for moving to a gzip build step.
  const size_t chrome = sizeof(WEB_PAGE_HEAD) + sizeof(WEB_PAGE_COUNT_FMT) +
                        sizeof(WEB_PAGE_BUTTON) + sizeof(WEB_PAGE_EMPTY) +
                        sizeof(WEB_PAGE_NO_CARD) + sizeof(WEB_PAGE_LIST_OPEN) +
                        sizeof(WEB_PAGE_LIST_CLOSE) + sizeof(WEB_PAGE_TAIL);
  printf("        chrome in flash: %zu bytes\n", chrome);
}

int main() {
  printf("\n=== note index HTML ===\n\n");
  testEscape();
  printf("\n");
  testEscapeRefusesRatherThanTruncates();
  printf("\n");
  testHref();
  printf("\n");
  testHrefSetIsInertInMarkup();
  printf("\n");
  testSize();
  printf("\n");
  testRow();
  printf("\n");
  testRowRefusesRatherThanTruncates();
  printf("\n");
  testWorstCaseRowFits();
  printf("\n");
  testAssembledListIsWellFormed();
  printf("\n");
  writePages();
  printf("\n");

  if (failures) {
    printf("FAILURES: %d\n", failures);
    return 1;
  }
  printf("note index HTML: every check passed\n");
  return 0;
}
