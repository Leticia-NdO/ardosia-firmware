#!/usr/bin/env python3
"""The notes page, read by an independent HTML parser.

test_note_html.cpp writes the three pages handleIndex() can produce
(src/web_page.h chrome + src/note_html.cpp rows). This reads them back with
Python's own parser and asserts the properties that matter on a stranger's
phone, where nothing on this device can observe a failure:

  * every tag is closed, and closed in order — an unclosed tag inside that
    <style> block renders as a blank page and reports nothing;
  * no <script> anywhere, because the page is specified to work with scripting
    off and the hotspot has no internet route to load one from (SPEC §4, §6);
  * every link goes to a note or to the archive, and nowhere else;
  * the markup carries no raw '<' or unescaped '&' from a note's name.

Same arrangement as check-zip.py: my own bytes checked against my own
expectations prove only that I am self-consistent.
"""

import html.parser
import sys

# Tags that legitimately have no closing tag in HTML5.
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
        "meta", "param", "source", "track", "wbr"}
# Tags whose closing tag HTML5 makes optional, and which this page omits.
OPTIONAL_CLOSE = {"html", "head", "body", "p", "li"}

failures = []


def fail(msg):
    failures.append(msg)
    print(f"  FAIL  {msg}")


def ok(msg):
    print(f"  ok    {msg}")


class Checker(html.parser.HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=False)
        self.stack = []
        self.counts = {}
        self.links = []
        self.mismatch = None

    def handle_starttag(self, tag, attrs):
        self.counts[tag] = self.counts.get(tag, 0) + 1
        if tag == "a":
            for k, v in attrs:
                if k == "href":
                    self.links.append(v)
        if tag not in VOID:
            self.stack.append(tag)

    def handle_endtag(self, tag):
        if tag in VOID:
            return
        # Pop through tags whose close is optional (<p>, <li>, <body>, …).
        while self.stack and self.stack[-1] != tag and self.stack[-1] in OPTIONAL_CLOSE:
            self.stack.pop()
        if not self.stack or self.stack[-1] != tag:
            if self.mismatch is None:
                self.mismatch = f"</{tag}> with {self.stack!r} open"
            return
        self.stack.pop()


def check_page(path, expect_rows, expect_zip_button):
    print(f"\n{path}")
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        fail(f"cannot read {path}: {e}")
        return

    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        fail(f"not valid UTF-8, which the charset meta promises: {e}")
        return
    ok("valid UTF-8, as the charset meta promises")

    c = Checker()
    c.feed(text)
    c.close()

    if c.mismatch:
        fail(f"tags do not nest: {c.mismatch}")
    else:
        ok("every tag closes, in order")

    left = [t for t in c.stack if t not in OPTIONAL_CLOSE]
    if left:
        fail(f"left open at the end: {left}")
    else:
        ok("nothing left open at the end")

    if "script" in c.counts or "<script" in text.lower():
        fail("a <script> tag: the page is specified to work with scripting off")
    else:
        ok("no script, so the download path cannot be broken by one")

    if c.counts.get("style", 0) != 1:
        fail(f"expected exactly one <style>, found {c.counts.get('style', 0)}")
    else:
        ok("exactly one inline stylesheet, nothing external to fetch")

    rows = c.counts.get("li", 0)
    if rows != expect_rows:
        fail(f"expected {expect_rows} <li> rows, found {rows}")
    else:
        ok(f"{rows} note rows")

    zip_links = [h for h in c.links if h == "/api/export.zip"]
    if expect_zip_button and len(zip_links) != 1:
        fail(f"expected one export button, found {len(zip_links)}")
    elif not expect_zip_button and zip_links:
        fail("an export button on a page with nothing to export")
    else:
        ok("the export button is where it should be")

    stray = [h for h in c.links
             if not h.startswith("/notes/") and h != "/api/export.zip"]
    if stray:
        fail(f"links that go somewhere unexpected: {stray}")
    else:
        ok("every link goes to a note or to the archive")

    # A note name that reached the markup unescaped would show up here, in the
    # text the parser hands back rather than in the tags it understood.
    for ref in _entityrefs(text):
        if ref not in ("amp", "lt", "gt", "middot"):
            fail(f"unexpected entity &{ref};")
            break
    else:
        ok("only the entities the escaper emits appear")


def _entityrefs(text):
    out = []
    i = 0
    while True:
        i = text.find("&", i)
        if i < 0:
            return out
        j = text.find(";", i)
        if j < 0 or j - i > 9:
            failures.append("a bare '&' in the markup")
            print(f"  FAIL  a bare '&' at offset {i}: {text[i:i+12]!r}")
            return out
        ref = text[i + 1:j]
        if not ref.startswith("#"):
            out.append(ref)
        i = j + 1


print("=== the notes page, read back by Python's HTML parser ===")
# The full page is written with four notes, one of which ("my note.txt") has a
# name no URL can carry: it must still be a row, just without a link.
check_page("/tmp/ardosia_page_full.html", expect_rows=5, expect_zip_button=True)
check_page("/tmp/ardosia_page_empty.html", expect_rows=0, expect_zip_button=False)
check_page("/tmp/ardosia_page_nocard.html", expect_rows=0, expect_zip_button=False)

print()
if failures:
    print(f"FAILURES: {len(failures)}")
    sys.exit(1)
print("the notes page: every check passed")
