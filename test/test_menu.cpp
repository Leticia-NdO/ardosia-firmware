// Host-side tests for the menu digit shortcuts.
//
// Typing 3 in a menu parks the selector on its third row ("[3] Settings" on the
// main menu, "3. Settings" in the list Settings still draws); Enter does the
// selecting. The mapping lives in config.h as menuDigit(), free of Arduino, so
// the decision of what counts as a shortcut can be checked here instead of
// costing an SD card write and a reboot.
//
// Run with test/run.sh — NOT part of the firmware build.

#include "../src/config.h"
#include "../src/keymap.h"

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

// ---------------------------------------------------------------------------

static void testDigitsMap() {
  printf("menuDigit: the number row\n");

  check(menuDigit(HID_KEY_1, 0) == 1, "1 selects row 1");
  check(menuDigit(0x1F, 0) == 2, "2 selects row 2");
  check(menuDigit(0x20, 0) == 3, "3 selects row 3 (Settings on the main menu)");
  check(menuDigit(0x25, 0) == 8, "8 selects row 8");
  check(menuDigit(HID_KEY_9, 0) == 9, "9 selects row 9");

  // 0 is the tenth row, not a tenth digit: the keycap order is 1..9 then 0, and
  // the HID usage table has the same order. Settings has ten rows since Editor
  // Font joined it, which is what made this worth having.
  check(menuDigit(HID_KEY_0, 0) == 10, "0 selects row 10");
  check(menuDigit(HID_KEY_0, 0) == MENU_MAX_NUMBERED,
        "and row 10 is the last row a digit can reach");

  // Numbering runs 1..9 top to bottom with no gaps: every usage between the
  // ends resolves, and consecutive keys give consecutive rows.
  for (uint8_t hid = HID_KEY_1; hid <= HID_KEY_9; hid++) {
    check(menuDigit(hid, 0) == (hid - HID_KEY_1) + 1, "number row is contiguous");
  }
}

static void testNonShortcuts() {
  printf("menuDigit: what is not a shortcut\n");

  // Immediately outside the range on both sides: Z is 0x1D, 0 is 0x27.
  check(menuDigit(0x1D, 0) == 0, "the usage below 1 (Z) is not a digit");
  check(menuDigit(0x28, 0) == 0, "the usage above 0 (Enter) is not a digit");

  check(menuDigit(HID_KEY_ENTER, 0) == 0, "Enter is not a shortcut");
  check(menuDigit(HID_KEY_ESCAPE, 0) == 0, "Esc is not a shortcut");
  check(menuDigit(HID_KEY_DOWN, 0) == 0, "Down is not a shortcut");
  check(menuDigit(HID_KEY_UP, 0) == 0, "Up is not a shortcut");
  check(menuDigit(HID_KEY_S, 0) == 0, "a letter is not a shortcut");
  check(menuDigit(0x00, 0) == 0, "an empty report is not a shortcut");
}

static void testModifiersDisqualify() {
  printf("menuDigit: modifiers\n");

  // Shift+1 is "!", AltGr+1 is "¹" on ABNT2, and Ctrl+1 is left free. None of
  // them should move the selector.
  check(menuDigit(HID_KEY_1, MOD_SHIFT_LEFT) == 0, "Shift+1 is ! , not a jump");
  check(menuDigit(HID_KEY_1, MOD_SHIFT_RIGHT) == 0, "right Shift disqualifies too");
  check(menuDigit(HID_KEY_1, MOD_CTRL_LEFT) == 0, "Ctrl+1 is not a jump");
  check(menuDigit(HID_KEY_1, MOD_CTRL_RIGHT) == 0, "right Ctrl disqualifies too");
  check(menuDigit(HID_KEY_1, MOD_ALT_LEFT) == 0, "Alt+1 is not a jump");
  check(menuDigit(HID_KEY_1, MOD_ALT_RIGHT) == 0, "AltGr+1 is ¹ on ABNT2, not a jump");
  check(menuDigit(HID_KEY_0, MOD_SHIFT_LEFT) == 0, "Shift+0 is ) , not row 10");
  check(menuDigit(HID_KEY_9, MOD_SHIFT_LEFT | MOD_CTRL_LEFT) == 0,
        "a combination disqualifies");

  // A bare press is the shortcut, and CapsLock is not a modifier byte, so it
  // cannot interfere.
  check(menuDigit(HID_KEY_1, 0) == 1, "an unmodified digit still jumps");
}

static void testLayoutIndependence() {
  printf("menuDigit: same keys on every layout\n");

  // The comment on menuDigit claims the number row sits at the same usages on
  // US, US-Intl and ABNT2, which is why the shortcut needs no layout lookup.
  // Check that against the real keymap rather than trusting the comment: if a
  // layout ever moved a digit, the shortcut would silently follow the wrong key.
  const KeyboardLayout layouts[] = {
      KeyboardLayout::US, KeyboardLayout::US_INTL, KeyboardLayout::ABNT2};

  for (KeyboardLayout layout : layouts) {
    for (uint8_t hid = HID_KEY_1; hid <= HID_KEY_9; hid++) {
      const KeyStroke ks = keymapResolve(layout, hid, 0, false);
      const uint32_t want = static_cast<uint32_t>('1' + (hid - HID_KEY_1));
      check(ks.cp == want, "the digit row prints the same digit on every layout");
      check(ks.mark == DeadMark::None, "a digit is never a dead key");
    }
    // 0 prints '0' everywhere too — it is excluded by choice, not by accident.
    check(keymapResolve(layout, HID_KEY_0, 0, false).cp == '0',
          "0 prints '0' on every layout");
  }
}

static void testNumberingReachesEveryRow() {
  printf("menu numbering: every row is reachable\n");

  // drawRowDigit() (the main menu) and numberedLabel() (Settings) both stop at
  // MENU_MAX_NUMBERED, and menuDigit resolves only 1..9. The two have to agree,
  // or the screen either shows a number nothing types or hides one that works.
  // Every digit key resolves to a row that actually gets a number drawn on it,
  // and none resolves past the last one.
  for (uint8_t hid = HID_KEY_1; hid <= HID_KEY_0; hid++) {
    const int row = menuDigit(hid, 0);
    check(row >= 1 && row <= MENU_MAX_NUMBERED, "every digit lands on a numbered row");
  }

  // Both menus have to fit inside the digits, or their tail rows are drawn
  // without numbers. These say so out loud before it happens.
  check(BASE_MENU_COUNT + MAX_OTA_APPS <= MENU_MAX_NUMBERED,
        "the fullest possible main menu still fits in the digits");
  // Settings numbers rows WITHIN a tab, so the bound is per tab — and with
  // three tabs it is not close, which is the point: the flat list had eleven
  // rows against ten digits and one row had to go without.
  for (int tab = 0; tab < SETTINGS_TAB_COUNT; tab++) {
    check(settingsTabRowCount(tab) <= MENU_MAX_NUMBERED,
          "every row of every Settings tab is reachable by a digit");
  }
}

// The Settings rows are named indices plus a switch that turns an index into a
// label. Those two are the pair that used to be a parallel array, where getting
// a reorder half-right showed one setting's name on another setting's row and
// nothing complained. These checks are what makes that loud.
static void testSettingsRows() {
  printf("settings rows: indices and labels agree\n");

  for (int row = 0; row < SETTINGS_COUNT; row++) {
    const char* label = settingLabel(row);
    check(label != nullptr && label[0] != '\0', "every row in range has a label");
  }

  // A row past the end has none — so a SETTINGS_COUNT raised without adding the
  // case shows up here rather than as a blank row on the device.
  check(settingLabel(SETTINGS_COUNT)[0] == '\0', "a row past the end has no label");
  check(settingLabel(-1)[0] == '\0', "a negative row has no label");

  // Every index is distinct: two constants sharing a value would make one row
  // unreachable and give another two names.
  for (int a = 0; a < SETTINGS_COUNT; a++) {
    for (int b = a + 1; b < SETTINGS_COUNT; b++) {
      check(strcmp(settingLabel(a), settingLabel(b)) != 0,
            "no two rows carry the same label");
    }
  }

  // The named constants have to cover exactly the range, with no gaps: a gap
  // would be a row with no behaviour behind it.
  const int rows[] = {SET_DARK_MODE, SET_EDITOR_FONT, SET_FONT_SIZE, SET_WRITING,
                      SET_NOTE_ORDER, SET_SLEEP,      SET_ORIENTATION,
                      SET_KEYBOARD,   SET_BLUETOOTH,  SET_PAIRED_KB,
                      SET_DPAD};
  check((int)(sizeof(rows) / sizeof(rows[0])) == SETTINGS_COUNT,
        "there are exactly SETTINGS_COUNT named rows");
  for (int want = 0; want < SETTINGS_COUNT; want++) {
    bool found = false;
    for (int i = 0; i < SETTINGS_COUNT; i++) {
      if (rows[i] == want) found = true;
    }
    check(found, "the row indices are 0..SETTINGS_COUNT-1 with no gaps");
  }

}

// The tabs. The table in config.h is the only thing that says where a row
// appears, so these checks are what stands between a typo in it and a setting
// that exists in the enum, has a label, has a handler — and is on no screen at
// all, on a device with no serial console to reach it from.
static void testSettingsTabs() {
  printf("settings tabs: every row has exactly one home\n");

  for (int tab = 0; tab < SETTINGS_TAB_COUNT; tab++) {
    check(settingsTabLabel(tab)[0] != '\0', "every tab has a name");
    check(settingsTabRowCount(tab) > 0, "and at least one row");
  }
  check(settingsTabLabel(SETTINGS_TAB_COUNT)[0] == '\0', "a tab past the end has no name");
  check(settingsTabLabel(-1)[0] == '\0', "nor does a negative one");
  check(settingsTabRowCount(SETTINGS_TAB_COUNT) == 0, "and no rows");
  check(settingsTabRowCount(-1) == 0, "in either direction");

  for (int a = 0; a < SETTINGS_TAB_COUNT; a++) {
    for (int b = a + 1; b < SETTINGS_TAB_COUNT; b++) {
      check(strcmp(settingsTabLabel(a), settingsTabLabel(b)) != 0,
            "no two tabs share a name");
    }
  }

  // THE property: every row lands in exactly one tab, exactly once. A row in no
  // tab is unreachable; a row in two tabs makes the digit that jumps to it
  // ambiguous and the selection invariant false.
  int total = 0;
  for (int tab = 0; tab < SETTINGS_TAB_COUNT; tab++) total += settingsTabRowCount(tab);
  check(total == SETTINGS_COUNT, "the tabs hold exactly SETTINGS_COUNT rows between them");

  for (int row = 0; row < SETTINGS_COUNT; row++) {
    int seen = 0;
    for (int tab = 0; tab < SETTINGS_TAB_COUNT; tab++) {
      const int n = settingsTabRowCount(tab);
      for (int pos = 0; pos < n; pos++) {
        if (settingsTabRow(tab, pos) == row) seen++;
      }
    }
    check(seen == 1, "each row appears in exactly one tab, exactly once");
  }

  // settingsPlaceOf is the inverse of settingsTabRow, both ways round. The
  // drawing code and the input code use opposite directions of this pair: the
  // screen turns a position into a row, the keys turn a row back into a
  // position. If they disagreed, the selector would sit on one row and Enter
  // would change another.
  for (int tab = 0; tab < SETTINGS_TAB_COUNT; tab++) {
    const int n = settingsTabRowCount(tab);
    for (int pos = 0; pos < n; pos++) {
      const int row = settingsTabRow(tab, pos);
      const SettingsPlace p = settingsPlaceOf(row);
      check(p.tab == tab && p.pos == pos, "place and row are inverses");
      check(settingLabel(row)[0] != '\0', "and every placed row has a label");
    }
    check(settingsTabRow(tab, n) == -1, "a position past the end of a tab is -1");
    check(settingsTabRow(tab, -1) == -1, "and so is a negative one");
  }
  check(settingsTabRow(SETTINGS_TAB_COUNT, 0) == -1, "a row of a tab that does not exist is -1");

  const SettingsPlace nowhere = settingsPlaceOf(SETTINGS_COUNT);
  check(nowhere.tab == -1 && nowhere.pos == -1, "a row that is not a row is placed nowhere");
  const SettingsPlace negative = settingsPlaceOf(-1);
  check(negative.tab == -1 && negative.pos == -1, "and neither is a negative one");

  // The two destinations — the screens you open when the keyboard has just
  // dropped — belong together and within reach. This is a judgement, not a
  // derivation, so it is asserted where it can be seen rather than left to a
  // comment nobody rereads.
  check(settingsPlaceOf(SET_BLUETOOTH).tab == TAB_CONTROLS, "Bluetooth is a control");
  check(settingsPlaceOf(SET_PAIRED_KB).tab == TAB_CONTROLS, "so are the paired keyboards");
  check(settingsPlaceOf(SET_DPAD).tab == TAB_CONTROLS, "and the d-pad");
  check(settingsPlaceOf(SET_KEYBOARD).tab == TAB_CONTROLS, "and the keyboard layout");
}

int main() {
  printf("\n=== menu digit shortcut tests ===\n\n");

  testDigitsMap();
  testNonShortcuts();
  testModifiersDisqualify();
  testLayoutIndependence();
  testNumberingReachesEveryRow();
  testSettingsRows();
  testSettingsTabs();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
