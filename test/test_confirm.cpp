// Host-side tests for the confirmation dialog.
//
// This is the guard in front of the two irreversible things the UI can do:
// delete a note and forget a keyboard bond. Testing it on the device would mean
// deleting notes on the device, so the decisions live in confirm.cpp, free of
// Arduino, and are checked here.
//
// The property that matters most is negative: NO single key, pressed once on a
// freshly opened dialog, destroys anything. Everything else is shape.
//
// Run with test/run.sh — NOT part of the firmware build.

#include "../src/confirm.h"

#include <cstdio>
#include <cstring>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
  checks++;
  if (!ok) {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

// Every key a screen behind the dialog could receive, plus the four the dialog
// itself uses. Used to sweep "what does an arbitrary key do".
static const uint8_t kKeys[] = {
    HID_KEY_ENTER, HID_KEY_ESCAPE, HID_KEY_UP, HID_KEY_DOWN, HID_KEY_LEFT,
    HID_KEY_RIGHT, HID_KEY_BACKSPACE, HID_KEY_DELETE, HID_KEY_TAB, HID_KEY_SPACE,
    HID_KEY_A, HID_KEY_D, HID_KEY_N, HID_KEY_S, 0x1C /* Y */, HID_KEY_1,
    HID_KEY_9, HID_KEY_0, 0x00, 0xFF};
static const int kKeyCount = (int)(sizeof(kKeys) / sizeof(kKeys[0]));

static const uint8_t kMods[] = {0, MOD_SHIFT_LEFT, MOD_CTRL_LEFT, MOD_ALT_RIGHT,
                                MOD_CTRL_LEFT | MOD_SHIFT_LEFT};
static const int kModCount = (int)(sizeof(kMods) / sizeof(kMods[0]));

static const ConfirmKind kKinds[] = {ConfirmKind::DeleteNote,
                                     ConfirmKind::DeleteOpenNote,
                                     ConfirmKind::ForgetKeyboard};

// ---------------------------------------------------------------------------

static void testClosedIsInert() {
  printf("confirm: a closed dialog answers nothing\n");

  ConfirmDialog d;
  check(!d.open(), "a fresh dialog is closed");

  for (int k = 0; k < kKeyCount; k++) {
    for (int m = 0; m < kModCount; m++) {
      const ConfirmReply r = confirmKey(d, kKeys[k], kMods[m]);
      check(r == ConfirmReply::Ignored, "every key on a closed dialog is Ignored");
      check(!d.open(), "and no key can open one");
    }
  }
}

static void testOpensOnCancel() {
  printf("confirm: it opens on Cancel, always\n");

  for (ConfirmKind kind : kKinds) {
    ConfirmDialog d;
    confirmOpen(d, kind, 7);
    check(d.open(), "open() is true once raised");
    check(d.kind == kind, "it remembers what it is asking about");
    check(d.index == 7, "and which row");
    check(!d.destructive, "the destructive button is NOT the one selected");

    // Reopening after the selection was moved must not inherit it: the box that
    // comes back has to start on Cancel like the first one did.
    d.destructive = true;
    confirmOpen(d, kind, 3);
    check(!d.destructive, "reopening resets the selection to Cancel");
    check(d.index == 3, "and captures the new row");
  }
}

// The one that matters: a person who opens the box and presses the key already
// under their finger loses nothing.
static void testOneKeyNeverDestroys() {
  printf("confirm: no single key on a fresh dialog destroys anything\n");

  for (ConfirmKind kind : kKinds) {
    for (int k = 0; k < kKeyCount; k++) {
      for (int m = 0; m < kModCount; m++) {
        ConfirmDialog d;
        confirmOpen(d, kind, 2);
        const ConfirmReply r = confirmKey(d, kKeys[k], kMods[m]);
        check(r != ConfirmReply::Confirmed,
              "one keystroke on a freshly opened dialog never confirms");
      }
    }
  }

  // Enter specifically — the key that opened half these screens — cancels.
  ConfirmDialog d;
  confirmOpen(d, ConfirmKind::DeleteNote, 0);
  check(confirmKey(d, HID_KEY_ENTER, 0) == ConfirmReply::Cancelled,
        "Enter on a fresh dialog cancels");
  check(!d.open(), "and closes it");
}

static void testArrowsSetRatherThanToggle() {
  printf("confirm: the arrows set the side they name\n");

  // Both pairs work, because both arrive: the physical buttons deliver Up and
  // Down on these screens, a keyboard user reaches for Left and Right at a
  // horizontal pair. And they SET — pressing Right twice must not walk back to
  // Cancel, which a toggle would do.
  const uint8_t toCancel[] = {HID_KEY_LEFT, HID_KEY_UP};
  const uint8_t toVerb[]   = {HID_KEY_RIGHT, HID_KEY_DOWN};

  for (uint8_t key : toVerb) {
    ConfirmDialog d;
    confirmOpen(d, ConfirmKind::DeleteNote, 1);
    check(confirmKey(d, key, 0) == ConfirmReply::Moved, "the first press moves");
    check(d.destructive, "onto the destructive button");
    check(confirmKey(d, key, 0) == ConfirmReply::Swallowed,
          "a second press changes nothing, so it asks for no repaint");
    check(d.destructive, "and stays there");
    check(d.open(), "an arrow never closes the dialog");
  }

  for (uint8_t key : toCancel) {
    ConfirmDialog d;
    confirmOpen(d, ConfirmKind::DeleteNote, 1);
    confirmKey(d, HID_KEY_RIGHT, 0);
    check(d.destructive, "(moved to the destructive side first)");
    check(confirmKey(d, key, 0) == ConfirmReply::Moved, "and back");
    check(!d.destructive, "onto Cancel");
    check(confirmKey(d, key, 0) == ConfirmReply::Swallowed,
          "pressing it again changes nothing");
    check(!d.destructive, "and stays on Cancel");
  }

  // Modifiers do not change what a button in the box does — Ctrl+Right is still
  // Right, because there is nothing else it could mean here.
  for (int m = 0; m < kModCount; m++) {
    ConfirmDialog d;
    confirmOpen(d, ConfirmKind::DeleteNote, 1);
    confirmKey(d, HID_KEY_RIGHT, kMods[m]);
    check(d.destructive, "a modifier does not disable the arrows");
  }
}

static void testConfirmAndCancelClose() {
  printf("confirm: both exits close the box\n");

  for (ConfirmKind kind : kKinds) {
    // Right, then Enter — the deliberate path.
    ConfirmDialog d;
    confirmOpen(d, kind, 5);
    confirmKey(d, HID_KEY_RIGHT, 0);
    check(confirmKey(d, HID_KEY_ENTER, 0) == ConfirmReply::Confirmed,
          "Enter on the destructive button confirms");
    check(!d.open(), "and the dialog is closed");
    // The reply can only be delivered once: a second Enter must not delete a
    // second note. This is what closing on the way out buys.
    check(confirmKey(d, HID_KEY_ENTER, 0) == ConfirmReply::Ignored,
          "a second Enter does nothing at all");

    // Esc from either side.
    confirmOpen(d, kind, 5);
    check(confirmKey(d, HID_KEY_ESCAPE, 0) == ConfirmReply::Cancelled,
          "Esc cancels from Cancel");
    check(!d.open(), "and closes");
    confirmOpen(d, kind, 5);
    confirmKey(d, HID_KEY_RIGHT, 0);
    check(confirmKey(d, HID_KEY_ESCAPE, 0) == ConfirmReply::Cancelled,
          "Esc cancels from the destructive button too");
    check(!d.open(), "and closes");

    // confirmClose from the outside — the path the power button takes when it
    // jumps to the main menu behind an open box.
    confirmOpen(d, kind, 5);
    confirmClose(d);
    check(!d.open(), "confirmClose closes it");
    check(!d.destructive, "and disarms the selection");
  }
}

static void testItIsModal() {
  printf("confirm: everything else is swallowed\n");

  // The notes list types-to-find. If the dialog let letters through, asking
  // whether to delete a note would go on filtering the list underneath — and
  // the index the box captured would start naming a different note. And if a
  // stray key CLOSED the box, the keystroke meant for the screen behind it
  // would dismiss the question instead.
  for (int k = 0; k < kKeyCount; k++) {
    const uint8_t key = kKeys[k];
    if (key == HID_KEY_ENTER || key == HID_KEY_ESCAPE || key == HID_KEY_UP ||
        key == HID_KEY_DOWN || key == HID_KEY_LEFT || key == HID_KEY_RIGHT) {
      continue;
    }
    for (int m = 0; m < kModCount; m++) {
      ConfirmDialog d;
      confirmOpen(d, ConfirmKind::DeleteNote, 4);
      const ConfirmReply r = confirmKey(d, key, kMods[m]);
      check(r == ConfirmReply::Swallowed, "an unrelated key is swallowed");
      check(d.open(), "and does not dismiss the dialog");
      check(d.index == 4, "nor move what it is asking about");
      check(!d.destructive, "nor arm it");
    }
  }
}

static void testIndexSurvives() {
  printf("confirm: the captured row survives the whole exchange\n");

  // What is drawn in the box is the name of d.index, and what the call site
  // deletes is d.index. So the value has to come out the far end of an
  // arbitrary sequence of keys unchanged, or the box asks about one note and
  // the code destroys another.
  for (int k = 0; k < kKeyCount; k++) {
    ConfirmDialog d;
    confirmOpen(d, ConfirmKind::DeleteNote, 11);
    confirmKey(d, kKeys[k], 0);
    if (d.open()) check(d.index == 11, "the captured row does not move");
  }

  // Including on the confirming keystroke, which is the one that uses it.
  ConfirmDialog d;
  confirmOpen(d, ConfirmKind::DeleteNote, 11);
  confirmKey(d, HID_KEY_DOWN, 0);
  int captured = d.index;
  check(confirmKey(d, HID_KEY_ENTER, 0) == ConfirmReply::Confirmed, "confirms");
  check(captured == 11, "with the row it opened on");
}

static void testWords() {
  printf("confirm: every kind has words\n");

  for (ConfirmKind kind : kKinds) {
    check(confirmQuestion(kind)[0] != '\0', "the question is not empty");
    check(confirmVerb(kind)[0] != '\0', "the button has a verb");
    // The verb is the destructive one, so it must never read as the harmless
    // button beside it.
    check(strcmp(confirmVerb(kind), "Cancel") != 0, "and it is not 'Cancel'");
  }
  check(strcmp(confirmQuestion(ConfirmKind::DeleteNote),
               confirmQuestion(ConfirmKind::ForgetKeyboard)) != 0,
        "deleting and forgetting ask different questions");
  check(strcmp(confirmVerb(ConfirmKind::DeleteNote),
               confirmVerb(ConfirmKind::ForgetKeyboard)) != 0,
        "and name different verbs");
  // The two delete kinds are deliberately the same words: from the reader's
  // side it is the same question about the same note, and only the code behind
  // the Yes differs (a row of the list, or the note open in the editor).
  check(strcmp(confirmQuestion(ConfirmKind::DeleteNote),
               confirmQuestion(ConfirmKind::DeleteOpenNote)) == 0,
        "the two delete kinds ask the same question");
  check(strcmp(confirmVerb(ConfirmKind::DeleteNote),
               confirmVerb(ConfirmKind::DeleteOpenNote)) == 0,
        "with the same verb");
  check(confirmQuestion(ConfirmKind::None)[0] == '\0', "None has no question");
  check(confirmVerb(ConfirmKind::None)[0] == '\0', "and no verb");
}

int main() {
  printf("\n=== confirmation dialog tests ===\n\n");

  testClosedIsInert();
  testOpensOnCancel();
  testOneKeyNeverDestroys();
  testArrowsSetRatherThanToggle();
  testConfirmAndCancelClose();
  testItIsModal();
  testIndexSurvives();
  testWords();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
