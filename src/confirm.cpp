#include "confirm.h"

void confirmOpen(ConfirmDialog& d, ConfirmKind kind, int index) {
  d.kind = kind;
  d.index = index;
  // Always Cancel. This is the whole point of the dialog: the key that is
  // already under the finger — Enter, which is what opened half these screens —
  // must land on the harmless button.
  d.destructive = false;
}

void confirmClose(ConfirmDialog& d) {
  d.kind = ConfirmKind::None;
  d.index = -1;
  d.destructive = false;
}

ConfirmReply confirmKey(ConfirmDialog& d, uint8_t hid, uint8_t mod) {
  if (!d.open()) return ConfirmReply::Ignored;
  (void)mod;   // a modifier never changes what a button in this box does

  // The two buttons are drawn side by side, Cancel first. The arrows SET the
  // selection rather than toggling it: the physical buttons only ever deliver
  // Up and Down here (in the notes list Left and Right are aliases for them),
  // while a keyboard user reaches for Left and Right at a horizontal pair. Both
  // pairs have to work, and a toggle would make the result depend on how many
  // presses arrived rather than on which one did.
  switch (hid) {
    case HID_KEY_LEFT:
    case HID_KEY_UP: {
      const bool changed = d.destructive;
      d.destructive = false;
      return changed ? ConfirmReply::Moved : ConfirmReply::Swallowed;
    }
    case HID_KEY_RIGHT:
    case HID_KEY_DOWN: {
      const bool changed = !d.destructive;
      d.destructive = true;
      return changed ? ConfirmReply::Moved : ConfirmReply::Swallowed;
    }
    case HID_KEY_ENTER: {
      const bool yes = d.destructive;
      confirmClose(d);
      return yes ? ConfirmReply::Confirmed : ConfirmReply::Cancelled;
    }
    case HID_KEY_ESCAPE:
      confirmClose(d);
      return ConfirmReply::Cancelled;
    default:
      // Everything else is eaten. A modal that let other keys through would go
      // on filtering the notes list underneath itself while asking whether to
      // delete one of its rows — and a dialog that closed on any stray key
      // would be dismissed by the keystroke that was meant for the screen
      // behind it. There are two ways out and both are drawn in the box.
      return ConfirmReply::Swallowed;
  }
}

const char* confirmQuestion(ConfirmKind kind) {
  switch (kind) {
    case ConfirmKind::DeleteNote:
    case ConfirmKind::DeleteOpenNote:  return "Delete this note?";
    case ConfirmKind::ForgetKeyboard:  return "Forget this keyboard?";
    default:                           return "";
  }
}

const char* confirmVerb(ConfirmKind kind) {
  switch (kind) {
    case ConfirmKind::DeleteNote:
    case ConfirmKind::DeleteOpenNote:  return "Delete";
    case ConfirmKind::ForgetKeyboard:  return "Forget";
    default:                           return "";
  }
}
