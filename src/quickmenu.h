#pragma once

// The quick menu: a short modal list raised by HOLDING the select button on the
// notes list or in the editor (Ctrl+K from a keyboard).
//
// Free of Arduino and of the renderer, like confirm.cpp beside it, so
// test/run.sh can exercise it. What each item DOES is the caller's business;
// this file owns which items exist, in what order, how the selector moves, and
// which items close the menu behind them.
//
// WHY A HOLD AND NOT A KEY: the case has six buttons and no modifier. Every
// shortcut the editor has — rename, font, writing mode — is a Ctrl chord, which
// means none of them exists for someone using the device without a keyboard.
// The hold is the only gesture left on the hardware.

#include "config.h"
#include <cstdint>

enum class QuickKind : uint8_t { None = 0, Note, Editor };

enum class QuickItem : uint8_t {
  Typeface = 0,   // Sans / Mono
  FontSize,
  DarkMode,
  WritingMode,
  Rename,
  Delete,
  Cancel,
  Count
};

enum class QuickReply : uint8_t {
  Ignored,    // no menu open — the caller handles the key as usual
  Swallowed,  // consumed, nothing changed on screen
  Moved,      // the selector moved; repaint
  Chosen,     // act on `item`; the menu may or may not still be open
  Cancelled   // closed with nothing to do
};

struct QuickMenu {
  QuickKind kind = QuickKind::None;

  // The notes-list row the menu was raised on, captured when it opened, so what
  // the box names is what the action lands on. -1 in the editor, where the
  // target is simply the open note.
  int index = -1;

  int selection = 0;

  bool open() const { return kind != QuickKind::None; }
};

struct QuickResult {
  QuickReply reply;
  QuickItem item;     // meaningful only when reply == Chosen
};

void quickOpen(QuickMenu& m, QuickKind kind, int index);
void quickClose(QuickMenu& m);

QuickResult quickKey(QuickMenu& m, uint8_t hid, uint8_t mod);

// The items a kind offers, in the order they are drawn. One table, like the
// Settings tabs: an item that is in no list is an action with no way in.
int quickItemCount(QuickKind kind);
QuickItem quickItemAt(QuickKind kind, int pos);

const char* quickItemLabel(QuickItem item);

// Whether choosing this item puts the menu away. The four that cycle a value
// leave it up: the editor is behind the box, so the row itself is the only
// place the new value can be read, and cycling three sizes should not mean
// raising the menu three times.
bool quickItemClosesMenu(QuickItem item);
