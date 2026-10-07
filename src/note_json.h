#pragma once

#include <stddef.h>

// --- The JSON shape of the note list ----------------------------------------
//
// Pure, so it can be exercised on the host: no Arduino, no SD, no WebServer.
// Same split, same reason, as deadkeys.* / hatch.* / ota_names.*.
//
// The reason this moved out of wifi_sync.cpp: handleFileList() used to build the
// WHOLE response in a heap String and then hand it to server->send(), which
// copies it again. At 40-odd bytes an entry that is ~8 KB for 200 notes and
// ~39 KB for 1000, doubled by the copy — on a device whose heap is already
// holding the AP, the HTTP server, a 16 KB text buffer and the NimBLE stack,
// and where String grows by realloc so the cost is the fragmentation as much as
// the peak. Streaming needs the per-entry formatting as a function, and once it
// is a function it may as well be a tested one.

// Escape a string into a JSON string body (no surrounding quotes), appending to
// whatever is already in `buf`. Quotes and backslashes are escaped; everything
// else, UTF-8 included, passes through.
//
// Moved here from sd_backup.h unchanged, so the two callers in ble_keyboard.cpp
// keep working and there is one escaper rather than two. This is the fix for
// finding F5 — a note dropped on the card by hand holding a quote used to break
// the whole response for every note after it — and until now it had no test.
void jsonAppendStr(char* buf, size_t bufSize, const char* src);

// Should this directory entry appear in the note list?
//
// Hidden files are skipped, and so is anything that is not .txt: the card is
// shared with the sleep wallpapers, the settings backup and the note-order
// sidecar, none of which are notes.
bool noteJsonIncludes(const char* name);

// Format one entry — `{"name":"…","size":N}` — with a leading comma when it is
// not the first. Returns the number of bytes written, or **0 when it would not
// fit**.
//
// Zero means "skip this entry", and the caller must honour that rather than
// write a partial one. A truncated entry does not lose one note, it corrupts
// the array from that point on and takes every note after it with it — which is
// exactly the failure F5 was: valid-looking JSON that stops making sense
// halfway. Dropping an entry is survivable; a broken response is not.
//
// `out` needs ~576 bytes for the longest name the card can hold (255 bytes, all
// of them quotes, so 510 escaped) plus the fixed parts.
size_t noteJsonEntry(const char* name, unsigned long size, bool first, char* out,
                     size_t n);

// What noteJsonEntry() needs in the worst case, so callers do not guess.
static constexpr size_t NOTE_JSON_ENTRY_MAX = 576;
