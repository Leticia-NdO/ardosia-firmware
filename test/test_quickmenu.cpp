// Host-side tests for the quick menu — the short modal list raised by holding
// the select button on the notes list or in the editor.
//
// Two of its seven items are irreversible, and the editor is live behind the
// box, so the properties worth pinning are the same shape as the confirmation
// dialog's: nothing destructive under the finger when it opens, nothing leaks
// through to the screen underneath, and the row it was raised on survives.
//
// Run with test/run.sh — NOT part of the firmware build.

#include "../src/quickmenu.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

static int failures = 0;
static int checks = 0;

static void check(bool ok, const char* what) {
  checks++;
  if (!ok) {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static const uint8_t kKeys[] = {
    HID_KEY_ENTER, HID_KEY_ESCAPE, HID_KEY_UP, HID_KEY_DOWN, HID_KEY_LEFT,
    HID_KEY_RIGHT, HID_KEY_BACKSPACE, HID_KEY_DELETE, HID_KEY_TAB, HID_KEY_SPACE,
    HID_KEY_A, HID_KEY_D, HID_KEY_N, HID_KEY_S, HID_KEY_1, HID_KEY_0, 0x00, 0xFF};
static const int kKeyCount = (int)(sizeof(kKeys) / sizeof(kKeys[0]));

static const uint8_t kMods[] = {0, MOD_SHIFT_LEFT, MOD_CTRL_LEFT, MOD_ALT_RIGHT};
static const int kModCount = (int)(sizeof(kMods) / sizeof(kMods[0]));

static const QuickKind kKinds[] = {QuickKind::Note, QuickKind::Editor};

// ---------------------------------------------------------------------------

static void testClosedIsInert() {
  printf("quick: a closed menu answers nothing\n");

  QuickMenu m;
  check(!m.open(), "a fresh menu is closed");
  for (int k = 0; k < kKeyCount; k++) {
    for (int mo = 0; mo < kModCount; mo++) {
      const QuickResult r = quickKey(m, kKeys[k], kMods[mo]);
      check(r.reply == QuickReply::Ignored, "every key on a closed menu is Ignored");
      check(!m.open(), "and no key can open one");
    }
  }
}

static void testItemTables() {
  printf("quick: the item tables\n");

  for (QuickKind kind : kKinds) {
    const int n = quickItemCount(kind);
    check(n > 0, "every kind offers something");

    // Every position gives a real item with a label, and no item repeats — a
    // duplicate would give the same action two rows and hide another.
    for (int a = 0; a < n; a++) {
      const QuickItem ia = quickItemAt(kind, a);
      check(ia != QuickItem::Count, "every position holds an item");
      check(quickItemLabel(ia)[0] != '\0', "and every item has a label");
      for (int b = a + 1; b < n; b++) {
        check(ia != quickItemAt(kind, b), "no item appears twice in one menu");
      }
    }
    check(quickItemAt(kind, n) == QuickItem::Count, "a position past the end is nothing");
    check(quickItemAt(kind, -1) == QuickItem::Count, "and so is a negative one");

    // Cancel is present and is the LAST row, so the selector neither starts nor
    // ends on Delete: the row under the finger when the box opens is harmless,
    // and so is the one a wrap-around lands on.
    check(quickItemAt(kind, n - 1) == QuickItem::Cancel, "Cancel is the last row");
    check(quickItemAt(kind, 0) != QuickItem::Delete, "and Delete is never the first");
  }

  check(quickItemCount(QuickKind::None) == 0, "the closed kind offers nothing");
  check(quickItemAt(QuickKind::None, 0) == QuickItem::Count, "and holds no items");

  // Only the three that leave the screen put the menu away. The four that cycle
  // a value stay up, because the row is the only place the new value can be
  // read with the editor hidden behind the box.
  check(quickItemClosesMenu(QuickItem::Rename), "Rename closes the menu");
  check(quickItemClosesMenu(QuickItem::Delete), "Delete closes the menu");
  check(quickItemClosesMenu(QuickItem::Cancel), "Cancel closes the menu");
  check(!quickItemClosesMenu(QuickItem::FontSize), "Font Size leaves it up");
  check(!quickItemClosesMenu(QuickItem::Typeface), "so does Typeface");
  check(!quickItemClosesMenu(QuickItem::DarkMode), "so does Dark Mode");
  check(!quickItemClosesMenu(QuickItem::WritingMode), "so does Writing Mode");
}

static void testOpensSomewhereHarmless() {
  printf("quick: it opens on a harmless row\n");

  for (QuickKind kind : kKinds) {
    QuickMenu m;
    quickOpen(m, kind, 4);
    check(m.open(), "open() is true once raised");
    check(m.index == 4, "it captures the row it was raised on");
    check(m.selection == 0, "and starts at the top");
    check(quickItemAt(kind, m.selection) != QuickItem::Delete,
          "which is never Delete");

    // A single Enter on a freshly raised menu never destroys anything.
    const QuickResult r = quickKey(m, HID_KEY_ENTER, 0);
    check(!(r.reply == QuickReply::Chosen && r.item == QuickItem::Delete),
          "one keystroke on a fresh menu never chooses Delete");

    // Reopening resets the selector rather than inheriting where it was left.
    quickOpen(m, kind, 9);
    check(m.selection == 0, "reopening starts at the top again");
    check(m.index == 9, "with the new row captured");
  }
}

static void testNavigationWraps() {
  printf("quick: the selector walks and wraps\n");

  for (QuickKind kind : kKinds) {
    const int n = quickItemCount(kind);
    QuickMenu m;
    quickOpen(m, kind, 0);

    // Down n times comes back to the top: no row is skipped and none repeats.
    for (int i = 0; i < n; i++) {
      check(m.selection == i, "Down walks the list one row at a time");
      const QuickResult r = quickKey(m, HID_KEY_DOWN, 0);
      check(r.reply == QuickReply::Moved, "and reports the move");
    }
    check(m.selection == 0, "and wraps to the top");

    // Up from the top goes to the bottom.
    quickKey(m, HID_KEY_UP, 0);
    check(m.selection == n - 1, "Up from the top wraps to the last row");

    // Every row is reachable, and the walk never closes the menu.
    check(m.open(), "walking never closes the menu");
  }
}

static void testChoosing() {
  printf("quick: what choosing does\n");

  // A cycling item is reported and the menu stays up, so the next Enter can
  // cycle it again without raising the box a second time.
  QuickMenu m;
  quickOpen(m, QuickKind::Editor, -1);
  const QuickResult first = quickKey(m, HID_KEY_ENTER, 0);
  check(first.reply == QuickReply::Chosen, "Enter on a cycling row chooses it");
  check(first.item == quickItemAt(QuickKind::Editor, 0), "and names that row");
  check(m.open(), "and the menu stays up");
  const QuickResult again = quickKey(m, HID_KEY_ENTER, 0);
  check(again.reply == QuickReply::Chosen && again.item == first.item,
        "a second Enter cycles the same row again");

  // Rename and Delete report themselves and close, so the action can only be
  // delivered once per gesture.
  for (QuickKind kind : kKinds) {
    for (QuickItem want : {QuickItem::Rename, QuickItem::Delete}) {
      QuickMenu q;
      quickOpen(q, kind, 3);
      const int n = quickItemCount(kind);
      for (int i = 0; i < n; i++) {
        if (quickItemAt(kind, q.selection) == want) break;
        quickKey(q, HID_KEY_DOWN, 0);
      }
      check(quickItemAt(kind, q.selection) == want, "(walked to the row)");
      const int captured = q.index;
      const QuickResult r = quickKey(q, HID_KEY_ENTER, 0);
      check(r.reply == QuickReply::Chosen && r.item == want, "it is chosen");
      check(!q.open(), "and the menu closes behind it");
      check(captured == 3, "with the row it was raised on intact");
      check(quickKey(q, HID_KEY_ENTER, 0).reply == QuickReply::Ignored,
            "so a second Enter cannot deliver it twice");
    }
  }

  // Cancel reports Cancelled, not Chosen: the caller must not have to know that
  // "chose Cancel" means "do nothing".
  for (QuickKind kind : kKinds) {
    QuickMenu q;
    quickOpen(q, kind, 0);
    const int n = quickItemCount(kind);
    for (int i = 0; i < n - 1; i++) quickKey(q, HID_KEY_DOWN, 0);
    check(quickItemAt(kind, q.selection) == QuickItem::Cancel, "(on Cancel)");
    check(quickKey(q, HID_KEY_ENTER, 0).reply == QuickReply::Cancelled,
          "Enter on Cancel reports Cancelled");
    check(!q.open(), "and closes");
  }
}

static void testItIsModal() {
  printf("quick: everything else is swallowed\n");

  // The editor is live under this box. A key that fell through would be typed
  // into the note; a key that closed the box would dismiss it by accident. Left
  // and Right matter especially: the editor's key repeat keeps firing whichever
  // direction button is held down.
  for (int k = 0; k < kKeyCount; k++) {
    const uint8_t key = kKeys[k];
    if (key == HID_KEY_ENTER || key == HID_KEY_ESCAPE || key == HID_KEY_UP ||
        key == HID_KEY_DOWN) {
      continue;
    }
    for (int mo = 0; mo < kModCount; mo++) {
      for (QuickKind kind : kKinds) {
        QuickMenu m;
        quickOpen(m, kind, 2);
        const QuickResult r = quickKey(m, key, kMods[mo]);
        check(r.reply == QuickReply::Swallowed, "an unrelated key is swallowed");
        check(m.open(), "and does not dismiss the menu");
        check(m.selection == 0, "nor move the selector");
        check(m.index == 2, "nor change what it was raised on");
      }
    }
  }

  // Esc is the other way out, from any row.
  for (QuickKind kind : kKinds) {
    const int n = quickItemCount(kind);
    for (int pos = 0; pos < n; pos++) {
      QuickMenu m;
      quickOpen(m, kind, 0);
      for (int i = 0; i < pos; i++) quickKey(m, HID_KEY_DOWN, 0);
      check(quickKey(m, HID_KEY_ESCAPE, 0).reply == QuickReply::Cancelled,
            "Esc cancels from any row");
      check(!m.open(), "and closes the menu");
    }
  }
}

int main() {
  printf("\n=== quick menu tests ===\n\n");

  testClosedIsInert();
  testItemTables();
  testOpensSomewhereHarmless();
  testNavigationWraps();
  testChoosing();
  testItIsModal();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
