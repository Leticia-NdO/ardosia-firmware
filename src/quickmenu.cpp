#include "quickmenu.h"

void quickOpen(QuickMenu& m, const QuickKind kind, const int index) {
  m.kind = kind;
  m.index = index;
  m.selection = 0;
}

void quickClose(QuickMenu& m) {
  m.kind = QuickKind::None;
  m.index = -1;
  m.selection = 0;
}

int quickItemCount(const QuickKind kind) {
  switch (kind) {
    case QuickKind::Note:   return 3;
    case QuickKind::Editor: return 7;
    default:                return 0;
  }
}

QuickItem quickItemAt(const QuickKind kind, const int pos) {
  // THE table. The notes list already has Ctrl+N and Ctrl+D, so its menu is the
  // same two actions for someone with no keyboard. The editor's menu is longer
  // because the editor has more behind chords: everything here except Rename
  // and Delete is otherwise reachable only as Ctrl+something.
  //
  // Cycling items first, in the order they are reached for; the destructive one
  // second to last, with Cancel under it so the selector never starts or ends
  // on Delete.
  static const QuickItem kNote[] = {QuickItem::Rename, QuickItem::Delete,
                                    QuickItem::Cancel};
  static const QuickItem kEditor[] = {QuickItem::FontSize, QuickItem::Typeface,
                                      QuickItem::DarkMode, QuickItem::WritingMode,
                                      QuickItem::Rename,   QuickItem::Delete,
                                      QuickItem::Cancel};
  const int n = quickItemCount(kind);
  if (pos < 0 || pos >= n) return QuickItem::Count;
  return (kind == QuickKind::Note) ? kNote[pos] : kEditor[pos];
}

const char* quickItemLabel(const QuickItem item) {
  switch (item) {
    case QuickItem::Typeface:    return "Typeface";
    case QuickItem::FontSize:    return "Font Size";
    case QuickItem::DarkMode:    return "Dark Mode";
    case QuickItem::WritingMode: return "Writing Mode";
    case QuickItem::Rename:      return "Rename";
    case QuickItem::Delete:      return "Delete";
    case QuickItem::Cancel:      return "Cancel";
    default:                     return "";
  }
}

bool quickItemClosesMenu(const QuickItem item) {
  switch (item) {
    case QuickItem::Rename:
    case QuickItem::Delete:
    case QuickItem::Cancel:
      return true;
    default:
      return false;   // the four that cycle a value in place
  }
}

QuickResult quickKey(QuickMenu& m, const uint8_t hid, const uint8_t mod) {
  if (!m.open()) return QuickResult{QuickReply::Ignored, QuickItem::Count};
  (void)mod;

  const int n = quickItemCount(m.kind);
  if (n <= 0) {   // a kind with no items cannot be navigated out of; close it
    quickClose(m);
    return QuickResult{QuickReply::Cancelled, QuickItem::Count};
  }

  switch (hid) {
    case HID_KEY_UP:
      m.selection = (m.selection - 1 + n) % n;
      return QuickResult{QuickReply::Moved, QuickItem::Count};
    case HID_KEY_DOWN:
      m.selection = (m.selection + 1) % n;
      return QuickResult{QuickReply::Moved, QuickItem::Count};

    case HID_KEY_ENTER: {
      const QuickItem item = quickItemAt(m.kind, m.selection);
      if (item == QuickItem::Cancel) {
        quickClose(m);
        return QuickResult{QuickReply::Cancelled, item};
      }
      if (quickItemClosesMenu(item)) quickClose(m);
      return QuickResult{QuickReply::Chosen, item};
    }

    case HID_KEY_ESCAPE:
      quickClose(m);
      return QuickResult{QuickReply::Cancelled, QuickItem::Count};

    default:
      // Modal, for the same reason the confirmation box is: the editor is
      // behind this menu, and a key that fell through would be typed into the
      // note under it. Left and Right land here too — the editor's key repeat
      // keeps firing whichever direction button is held, and only up and down
      // mean anything to a list.
      return QuickResult{QuickReply::Swallowed, QuickItem::Count};
  }
}
