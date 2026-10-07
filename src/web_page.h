#pragma once

// --- The notes page, as flash ------------------------------------------------
//
// One page, served at GET / by handleIndex() in wifi_sync.cpp. Read-only:
// a list of titles, a link per note, and one button that takes everything as a
// ZIP (../SPEC-notes-export.md §6).
//
// WHY A RAW LITERAL AND NOT SPIFFS. The partition table does declare a 3.5 MB
// spiffs partition and nothing in this firmware touches it. Tempting, and
// wrong: the whole distribution model is ONE .bin on a device with no USB
// (SPEC-firmware-distribution.md). A page in SPIFFS is a second artifact, got
// there by a second mechanism, and it breaks the recovery story that this
// entire cycle was spent making solid. So: const char[] in .rodata, which is
// memory-mapped flash and costs no RAM.
//
// WHY NO BUILD STEP. CrossInk composes, minifies and gzips its web assets with
// scripts/build_web.py and serves them with Content-Encoding: gzip. That is the
// right answer at its size and the wrong one at this one — this is a list and
// one button, and a generated header is a file that can be stale, a build order
// that can be wrong, and a diff nobody reads. Runtime gzip is not an option
// either: miniz is not an IDF 4.4 component here. The threshold for moving to
// (4b) in the spec is roughly 8 KB of payload or a second page; this is ~2.5 KB
// and one page.
//
// WHY NO JAVASCRIPT AT ALL. The hotspot has no internet route, so anything the
// page uses has to be in the page — no CDN, no font, no framework. And the
// download path is the whole point of the feature: plain <a href> links keep it
// working with a broken script, an old browser, or a cheap phone's built-in
// one. The list is therefore rendered by the device and streamed, which also
// means the page needs no /api/files round trip to show anything.
//
// The markup is split where the dynamic parts go. Everything is streamed with
// sendContent(), never assembled: same constraint as the note list and the
// archive (SPEC §8).
//
// `rel="icon" href="data:,"` is not decoration — it stops the browser asking
// for /favicon.ico, which this server would answer with a 404 over a soft AP.

static const char WEB_PAGE_HEAD[] = R"HTML(<!doctype html>
<html lang="en">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="data:,">
<title>Ardosia notes</title>
<style>
:root{color-scheme:light dark;--fg:#16181d;--bg:#fff;--mut:#6b7280;--rule:#d7dae0}
@media(prefers-color-scheme:dark){
:root{--fg:#e6e7ea;--bg:#15171b;--mut:#9aa0ab;--rule:#2b2f36}}
*{box-sizing:border-box}
body{margin:0 auto;padding:1.75rem 1rem 3rem;max-width:36rem;color:var(--fg);
background:var(--bg);font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
h1{margin:0;font-size:1.5rem;letter-spacing:-.01em}
.c{margin:.25rem 0 1.5rem;color:var(--mut);font-size:.9rem}
.b{display:block;padding:.85rem 1rem;border-radius:.5rem;background:var(--fg);
color:var(--bg);text-align:center;text-decoration:none;font-weight:600}
.b:active{opacity:.7}
ul{list-style:none;margin:1.75rem 0 0;padding:0;border-top:1px solid var(--rule)}
li{display:flex;gap:1rem;align-items:baseline;justify-content:space-between;
padding:.7rem .15rem;border-bottom:1px solid var(--rule)}
li a{color:inherit;font-weight:500;text-decoration:none}
li a:hover{text-decoration:underline}
.s{flex:none;color:var(--mut);font-size:.85rem;font-variant-numeric:tabular-nums}
.d{color:var(--mut)}
.e{margin:2rem 0;color:var(--mut)}
.f{margin:2rem 0 0;color:var(--mut);font-size:.8rem}
</style>
<h1>Ardosia</h1>
)HTML";

// The count line, then either the button or the empty state. printf format,
// because the count is the only number on the page.
static const char WEB_PAGE_COUNT_FMT[] = "<p class=\"c\">%d note%s on the device</p>\n";

static const char WEB_PAGE_BUTTON[] =
    "<a class=\"b\" href=\"/api/export.zip\">Download all notes (.zip)</a>\n";

static const char WEB_PAGE_EMPTY[] =
    "<p class=\"e\">Nothing to export yet. Notes written on the device "
    "show up here.</p>\n";

// An unreadable /notes is NOT the same thing as an empty one, and saying
// "nothing to export yet" when the card is the problem sends someone looking
// in the wrong place. This device has no serial console: if the page does not
// say it, nothing does.
static const char WEB_PAGE_NO_CARD[] =
    "<p class=\"e\">The notes folder could not be read. Check that the SD "
    "card is seated, then reload.</p>\n";

static const char WEB_PAGE_LIST_OPEN[]  = "<ul>\n";

// Separate from the tail, because the empty case emits no <ul> at all and a
// stray </ul> is the kind of markup bug a browser hides rather than reports.
static const char WEB_PAGE_LIST_CLOSE[] = "</ul>\n";

static const char WEB_PAGE_TAIL[] = R"HTML(<p class="f">Read-only page. Nothing here can change or delete a note.</p>
</html>
)HTML";
