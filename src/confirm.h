#pragma once

// The confirmation dialog for destructive actions.
//
// Free of Arduino and of the renderer, like keymap/deadkeys/ui_layout, so
// test/run.sh can exercise it: a dialog that guards a delete is exactly the
// kind of logic that must not be tested by deleting notes on the device.
//
// WHY THIS EXISTS, in two places that behaved differently:
//
//   Notes           Ctrl+D put "Delete? Enter:Yes Esc:No" in the FOOTER and set
//                   a flag. The question was a line of 9pt text at the bottom
//                   of the screen, and the next Enter deleted.
//   Paired keyboard `D` — a bare letter, no modifier — removed the bond on the
//                   spot, with no question at all. On the one screen you reach
//                   BECAUSE the keyboard is misbehaving.
//
// Both now raise the same modal box, and the box starts on Cancel: one blind
// Enter can never destroy anything.

#include "config.h"
#include <cstdint>

// What is being asked about. The kind chooses the words; the call site decides
// what actually happens on Yes.
// DeleteNote and DeleteOpenNote ask the same question and differ only in what
// the call site does with a yes: one deletes a row of the notes list, the other
// the note currently open in the editor — which has to disarm the editor before
// it deletes anything, or the autosave writes the note straight back.
enum class ConfirmKind : uint8_t { None = 0, DeleteNote, DeleteOpenNote, ForgetKeyboard };

// What a key did to the dialog.
enum class ConfirmReply : uint8_t {
  Ignored,    // no dialog is open — the caller handles the key as usual
  Swallowed,  // consumed, nothing changed on screen (a modal eats stray keys)
  Moved,      // the selection changed; repaint
  Confirmed,  // the destructive button was chosen; the dialog is now closed
  Cancelled   // closed with nothing to do
};

struct ConfirmDialog {
  ConfirmKind kind = ConfirmKind::None;

  // The row the question was asked ABOUT, captured when the dialog opened.
  // The call site acts on this, never on the live selection: what you were
  // shown the name of is what gets deleted, even if something moves the
  // selection underneath.
  int index = -1;

  // false = Cancel (where every dialog starts), true = the destructive button.
  bool destructive = false;

  bool open() const { return kind != ConfirmKind::None; }
};

void confirmOpen(ConfirmDialog& d, ConfirmKind kind, int index);
void confirmClose(ConfirmDialog& d);

// Feed it every key while a dialog may be open, and act on the reply. Returns
// Ignored when there is none, which is the caller's signal to carry on.
ConfirmReply confirmKey(ConfirmDialog& d, uint8_t hid, uint8_t mod);

// "Delete this note?" — the question, in the box's first line.
const char* confirmQuestion(ConfirmKind kind);

// "Delete" — the word on the destructive button.
const char* confirmVerb(ConfirmKind kind);
