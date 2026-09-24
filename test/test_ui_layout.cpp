// Host-side tests for the screen layout arithmetic.
//
// This is the math that produced "the words sit low and get clipped along the
// bottom" in every menu, and the math that the Bluetooth screen got wrong by
// stacking text at fixed y values until the lines printed over each other.
// Both are cheap to assert here and expensive to see on a device that needs an
// SD card write and a reboot per look.
//
// Run with test/run.sh — NOT part of the firmware build.

#include "../src/ui_layout.h"

#include <cstdio>
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

static void checkEq(int got, int want, const char* what) {
  checks++;
  if (got != want) {
    failures++;
    printf("  FAIL  %s\n        got  %d\n        want %d\n", what, got, want);
  }
}

// Real fonts, as measured off their tables. Keeping actual numbers here means
// a font swap that changes the shape of the problem shows up as a test failure
// rather than as a surprise on the glass.
//
// ubuntu_10 is no longer in the firmware — it left when the chrome went
// monospace — but it stays in this table because it is the case lineStepOf()
// exists for: its advanceY was exactly its ink height, so lines stacked with
// getLineHeight() came out touching.
//   ascender, descender, tallTop ('Á'), lowHeight ('g'), lowTop ('g')
struct FontRow { const char* name; int asc, desc, tallTop, lowH, lowTop; int wantInk; };
static const FontRow kFonts[] = {
    {"ubuntu_10",        20, -4, 20, 15, 11, 24},   // gone from the build; see above
    {"notosans_12",      27, -8, 24, 20, 14, 30},   // the editor's, at Font Size Small
    {"jetbrainsmono_9",  20,  -6, 19, 15, 11, 23},   // FONT_CHROME_S
    {"jetbrainsmono_11", 24,  -7, 23, 17, 13, 27},   // FONT_CHROME_M
    {"jetbrainsmono_13", 28,  -9, 26, 20, 15, 31},   // FONT_CHROME_L
    {"jetbrainsmono_18", 39, -12, 36, 28, 21, 43},   // FONT_CHROME_XL
    {"notosans_14",      32,  -9, 28, 23, 16, 35},   // the editor's, Font Size Medium
    {"notosans_16",      36, -10, 32, 27, 19, 40},   // the editor's, Font Size Large
};
static const int kChromeS = 2, kChromeM = 3, kChromeL = 4, kChromeXL = 5;
static const int kEditorLarge = 7;

static TextInk inkOf(const FontRow& f) {
  return inkFromMetrics(f.asc, f.desc, true, f.tallTop, true, f.lowH, f.lowTop);
}

// ---------------------------------------------------------------------------

static void testInkFromMetrics() {
  printf("inkFromMetrics\n");

  // The tallest accented capital starts where the ascender box ends above it.
  // A font whose 'Á' reaches the full ascender inks from row 0.
  const TextInk flush = inkFromMetrics(28, -9, true, 28, true, 16, 13);
  checkEq(flush.top, 0, "a glyph that reaches the ascender inks from row 0");
  checkEq(flush.bottom, 31, "bottom = ascender + how far 'g' hangs below baseline");
  checkEq(flush.height(), 31, "height is bottom - top");

  // A shorter cap leaves clear space at the top of the box.
  const TextInk shorter = inkFromMetrics(28, -9, true, 20, true, 16, 13);
  checkEq(shorter.top, 8, "a cap 8px below the ascender inks from row 8");

  // Without a 'g' to measure, the font's own descender stands in. It is
  // stored negative, so the depth is its negation.
  const TextInk noLow = inkFromMetrics(28, -9, true, 28, false, 0, 0);
  checkEq(noLow.bottom, 37, "no 'g': falls back on -descender");

  // Without any cap glyph at all, the ink is assumed to start at the top.
  const TextInk noTall = inkFromMetrics(28, -9, false, 0, true, 16, 13);
  checkEq(noTall.top, 0, "no cap glyph: ink starts at row 0");
}

static void testRealFonts() {
  printf("ink of the real fonts\n");
  for (const FontRow& f : kFonts) {
    checkEq(inkOf(f).height(), f.wantInk, f.name);
  }

  // fontconvert.py takes POINTS at 150 DPI, so a "size" is roughly 2.08 device
  // pixels. Getting that backwards is how the chrome first shipped at double
  // the intended size. These four are the design's 11/13/15/22px, and they have
  // to stay in that order and well under the editor's own text.
  check(inkOf(kFonts[kChromeS]).height() < inkOf(kFonts[kChromeM]).height(),
        "S is smaller than M");
  check(inkOf(kFonts[kChromeM]).height() < inkOf(kFonts[kChromeL]).height(),
        "M is smaller than L");
  check(inkOf(kFonts[kChromeL]).height() < inkOf(kFonts[kChromeXL]).height(),
        "L is smaller than XL");
  // The chrome grew past the editor's smallest setting, which is fine — that
  // setting is deliberately tiny. What must hold is that the chrome never
  // shouts over the writing at its largest: the note is the point of the device.
  check(inkOf(kFonts[kChromeL]).height() < inkOf(kFonts[kEditorLarge]).height(),
        "a chrome row label stays under the editor's largest text");
  check(inkOf(kFonts[kChromeXL]).height() > inkOf(kFonts[kEditorLarge]).height(),
        "the screen title is the one thing allowed to be bigger");
}

static void testBandGeometry() {
  printf("band and pitch\n");
  const TextInk ink = inkOf(kFonts[kChromeL]);

  checkEq(bandHeightOf(ink), ink.height() + 2 * ROW_PAD,
          "band = ink + padding on both sides");
  checkEq(rowPitchOf(ink), bandHeightOf(ink) + ROW_GAP, "pitch = band + gap");
  // Rows sit flush, as the design draws them: the separator is a rule, not a
  // gap. What must never happen is a pitch SMALLER than the band, which would
  // overlap one row's ink with the next.
  check(rowPitchOf(ink) >= bandHeightOf(ink), "rows never overlap");

  // lineStep must not collapse to the ink height. ubuntu_10's advanceY WAS
  // exactly its ink height, which is how stacked lines came out touching —
  // that is the trap this function exists to avoid.
  for (const FontRow& f : kFonts) {
    check(lineStepOf(inkOf(f)) > inkOf(f).height(), "lineStep leaves room between lines");
  }
}

static void testTextSitsInsideItsBand() {
  printf("textYInBand: the clipping bug\n");

  // The property that matters: whatever y comes back, every inked row has to
  // land inside [bandTop, bandTop + bandH). Checked across every shipped font
  // and a range of band sizes, including bands larger than the design asks for.
  for (const FontRow& f : kFonts) {
    const TextInk ink = inkOf(f);
    for (int bandH = ink.height(); bandH <= ink.height() + 40; bandH++) {
      for (int bandTop = 0; bandTop < 120; bandTop += 17) {
        const int y = textYInBandOf(ink, bandTop, bandH);
        const bool topOk = y + ink.top >= bandTop;
        const bool botOk = y + ink.bottom <= bandTop + bandH;
        if (!topOk || !botOk) {
          printf("  FAIL  %s ink=%d band=%d top=%d -> y=%d spills (%d..%d vs %d..%d)\n",
                 f.name, ink.height(), bandH, bandTop, y,
                 y + ink.top, y + ink.bottom, bandTop, bandTop + bandH);
          failures++;
        }
        checks++;
      }
    }
  }

  // Exact fit: a band the size of the ink leaves no slack anywhere.
  const TextInk ink = inkOf(kFonts[kChromeL]);
  const int y = textYInBandOf(ink, 100, ink.height());
  checkEq(y + ink.top, 100, "exact-fit band: ink starts at the band top");
  checkEq(y + ink.bottom, 100 + ink.height(), "exact-fit band: ink ends at the band bottom");

  // The y is NOT the band top: that is the whole bug. drawText() would put the
  // baseline an ascender below it and drop the descenders out of the band.
  check(textYInBandOf(ink, 100, ink.height() + 20) != 100,
        "y is not the band top — that is what clipped the descenders");

  // A band too small to hold the ink is not clamped: better to overflow
  // predictably than to pin the text to an edge and pretend it fits.
  const int tight = textYInBandOf(ink, 100, ink.height() - 10);
  checkEq(tight, 100 + (-10) / 2 - ink.top, "an undersized band centres without clamping");
}

static void testFrameGeometry() {
  printf("frame: geometry\n");

  // The content column IS the Figma's, unchanged: the horizontal axis was never
  // the constraint, so 760 of 800 and 440 of 480 stand as drawn.
  checkEq(contentXOf(), WORKSPACE_PAD, "content starts one workspace pad in");
  checkEq(contentWOf(800), 800 - 2 * WORKSPACE_PAD, "landscape content column");
  checkEq(contentWOf(480), 480 - 2 * WORKSPACE_PAD, "portrait content column");
  checkEq(contentWOf(0), 0, "a degenerate screen yields no content column");

  // Vertically the design was NOT taken literally — see the note on ROW_PAD.
  // The header is padding, the title's ink, padding, rule; what matters is that
  // it is derived from the ink rather than pinned to a number that a font
  // change would silently break.
  const TextInk title = inkOf(kFonts[kChromeXL]);
  const HeaderLayout h = headerLayoutOf(title);
  checkEq(h.height, 2 * HEADER_PAD_Y + title.height() + HEADER_RULE,
          "header = padding + title ink + padding + rule");
  checkEq(h.boxH, title.height(), "the header box is exactly the title's ink");
  check(h.ruleY + HEADER_RULE == FRAME_INSET + FRAME_BORDER + h.height,
        "the rule closes the header band");

  // The Figma's own 53px header would no longer hold the title. That is the
  // deliberate divergence, asserted so nobody "restores" it by hand later.
  check(h.height > 53, "the scaled title needs a taller header than the Figma's 53px");

  const TextInk foot = inkOf(kFonts[kChromeS]);
  const FooterLayout f1 = footerLayoutOf(480, foot, 760, 92, 268, 106, true);
  checkEq(f1.rows, 1, "at 760 the three columns fit on one line");
  checkEq(f1.height, FOOTER_RULE + 2 * FOOTER_PAD_Y + foot.height(),
          "a one-row footer is rule + padding + ink + padding");

  const FooterLayout f2 = footerLayoutOf(800, foot, 440, 92, 268, 106, true);
  checkEq(f2.rows, 2, "at 440 the middle column stacks");
  check(f2.height > f1.height, "stacking makes the footer taller");
}

// The scale-up exists to buy readable text without losing the list. If a future
// size change quietly costs rows, this is what says so.
static void testDensitySurvivesTheScale() {
  printf("frame: the scale-up keeps the lists usable\n");

  const TextInk ctx = inkOf(kFonts[kChromeS]);
  const HeaderLayout h = headerLayoutOf(inkOf(kFonts[kChromeXL]));
  const int pitch = rowPitchOf(inkOf(kFonts[kChromeL]));

  // Model what the screens actually do, context line included — an earlier
  // version of this check left it out and reported one row more than the
  // device draws.
  auto visibleRows = [&](int screenH, int contentW) {
    const FooterLayout f = footerLayoutOf(screenH, ctx, contentW, 92, 268, 106, true);
    const int top = bodyTopOf(h) + WORKSPACE_PAD;
    const int listTop = top + ctx.height() + WORKSPACE_PAD;   // below "MAIN MENU"
    return (bodyBottomOf(f) - WORKSPACE_PAD - listTop) / pitch;
  };

  // Landscape is the primary orientation and the binding constraint. The main
  // menu has 5 fixed entries plus the dual-boot slot; needing to scroll to
  // reach CrossPoint would be a regression worth failing over.
  check(visibleRows(480, 760) >= BASE_MENU_COUNT + 1,
        "landscape shows the fixed menu plus the dual-boot entry without scrolling");
  check(visibleRows(800, 440) >= 10, "portrait shows at least ten list rows");
}

static void testFooterStacking() {
  printf("footer: only the middle column reflows\n");
  const TextInk ink = inkOf(kFonts[kChromeS]);

  // Exactly at the limit it still fits; one pixel over and it stacks.
  const int fit = 92 + 268 + 106 + 2 * FOOTER_COL_GAP;
  checkEq(footerLayoutOf(480, ink, fit, 92, 268, 106, true).rows, 1,
          "an exact fit stays on one line");
  checkEq(footerLayoutOf(480, ink, fit - 1, 92, 268, 106, true).rows, 2,
          "one pixel short and it stacks");

  // A middle column with no stacked form never stacks, however tight.
  checkEq(footerLayoutOf(480, ink, 10, 92, 268, 106, false).rows, 1,
          "a column that cannot stack stays on one line and truncates instead");

  // No middle column at all: nothing to reflow, whatever the width.
  checkEq(footerLayoutOf(480, ink, 10, 92, 0, 106, true).rows, 1,
          "with no middle column the footer stays one row");
}

static void testFrameFitsTheScreen() {
  printf("frame: header, body and footer tile the screen\n");

  const TextInk title = inkOf(kFonts[kChromeXL]);
  const TextInk foot = inkOf(kFonts[kChromeS]);

  for (int screenH : {480, 800}) {
    for (int rowsWanted = 0; rowsWanted < 2; rowsWanted++) {
      const int contentW = (screenH == 480) ? 760 : 440;
      // Force one row or two by handing the middle column a width that fits or not.
      const FooterLayout f =
          footerLayoutOf(screenH, foot, contentW, 92, rowsWanted ? contentW * 4 : 10, 106, true);
      const HeaderLayout h = headerLayoutOf(title);

      const int top = bodyTopOf(h);
      const int bottom = bodyBottomOf(f);
      check(top < bottom, "the body box is not empty");
      check(top == FRAME_INSET + FRAME_BORDER + h.height,
            "the body starts where the header ends");
      check(bottom == f.ruleY, "the body ends at the footer rule");
      check(f.ruleY + f.height + FRAME_INSET + FRAME_BORDER == screenH,
            "the footer sits flush against the bottom border");
      check(bottom + f.height + FRAME_BORDER + FRAME_INSET == screenH,
            "nothing is left over below the footer");
    }
  }
}

static void testScrollWindow() {
  printf("scroll: the visible window\n");

  // A list that fits never scrolls, wherever the selection is.
  for (int sel = 0; sel < 6; sel++) {
    checkEq(scrollStartOf(6, 6, sel), 0, "a list that fits starts at 0");
    checkEq(scrollStartOf(3, 6, sel), 0, "a short list starts at 0");
  }

  // Settings is the case that prompted this: nine rows, six visible.
  checkEq(scrollStartOf(9, 6, 0), 0, "selection 0 shows the top of the list");
  checkEq(scrollStartOf(9, 6, 5), 0, "the last visible row does not scroll yet");
  checkEq(scrollStartOf(9, 6, 6), 1, "stepping past the window scrolls by one");
  checkEq(scrollStartOf(9, 6, 8), 3, "the last row parks the window at the end");

  // Never past the end: the window always stays full.
  checkEq(scrollStartOf(9, 6, 8), 9 - 6, "no blank rows below the last item");

  // Moving back up scrolls the other way, one row at a time.
  checkEq(scrollStartOf(9, 6, 2), 0, "coming back up returns to the top");

  // The invariant that matters: the selection is always drawn.
  for (int total = 1; total <= 40; total++) {
    for (int visible = 1; visible <= 12; visible++) {
      for (int sel = 0; sel < total; sel++) {
        const int start = scrollStartOf(total, visible, sel);
        const bool inWindow = (sel >= start) && (sel < start + visible);
        const bool inRange = (start >= 0) && (start + visible <= total || total <= visible);
        if (!inWindow || !inRange) {
          printf("  FAIL  total=%d visible=%d sel=%d -> start=%d\n", total, visible, sel, start);
          failures++;
        }
        checks++;
      }
    }
  }

  // Degenerate inputs must not produce a negative index.
  checkEq(scrollStartOf(0, 6, 0), 0, "an empty list starts at 0");
  checkEq(scrollStartOf(9, 0, 4), 0, "a window of no rows starts at 0");
}

static void testScrollThumb() {
  printf("scroll: the thumb\n");

  // A list that fits draws no track at all.
  checkEq(scrollThumbOf(300, 6, 6, 0).h, 0, "a list that fits has no thumb");
  checkEq(scrollThumbOf(300, 3, 6, 0).h, 0, "a short list has no thumb");
  checkEq(scrollThumbOf(0, 9, 6, 0).h, 0, "no track, no thumb");

  // Settings again: six of nine visible, so the thumb is two thirds.
  const ScrollThumb top = scrollThumbOf(300, 9, 6, 0);
  checkEq(top.h, 200, "the thumb is the visible fraction of the track");
  checkEq(top.y, 0, "at the top of the list the thumb is at the top");

  const ScrollThumb bottom = scrollThumbOf(300, 9, 6, 3);
  checkEq(bottom.y + bottom.h, 300, "at the end the thumb touches the bottom");

  // Where it sits in BETWEEN is the part the end points cannot pin down: a
  // thumb that jumps straight from top to bottom passes both checks above.
  // Halfway down a long list it belongs halfway down the free travel.
  const ScrollThumb mid = scrollThumbOf(300, 100, 10, 45);
  checkEq(mid.y, (300 - mid.h) / 2, "halfway through the list is halfway down the track");

  // And it only ever moves downward as the list scrolls.
  int prev = -1;
  for (int start = 0; start <= 90; start++) {
    const ScrollThumb t = scrollThumbOf(300, 100, 10, start);
    check(t.y >= prev, "the thumb never moves back up as the list scrolls down");
    prev = t.y;
  }

  // A track shorter than the minimum thumb: the thumb fills it rather than
  // overflowing. Sweeping only realistic track heights missed this entirely.
  for (int trackH = 1; trackH < SCROLLBAR_MIN_THUMB; trackH++) {
    const ScrollThumb t = scrollThumbOf(trackH, 20, 3, 5);
    checkEq(t.h, trackH, "a track shorter than the minimum thumb is filled, not overflowed");
    check(t.y + t.h <= trackH, "and the thumb still fits inside it");
  }

  // Never off the track, never thinner than the minimum, for any list shape.
  // A handful of track heights covering a squeezed list, a real one and a tall
  // portrait screen. Sweeping every pixel added a million assertions and two
  // seconds for no extra coverage.
  for (int trackH : {1, 5, 11, 12, 20, 47, 121, 300, 627}) {
    for (int total = 2; total <= 24; total++) {
      for (int visible = 1; visible < total; visible++) {
        for (int start = 0; start <= total - visible; start++) {
          const ScrollThumb t = scrollThumbOf(trackH, total, visible, start);
          const bool ok = t.h >= 1 && t.y >= 0 && t.y + t.h <= trackH &&
                          (t.h >= SCROLLBAR_MIN_THUMB || t.h == trackH);
          if (!ok) {
            printf("  FAIL  track=%d total=%d visible=%d start=%d -> y=%d h=%d\n",
                   trackH, total, visible, start, t.y, t.h);
            failures++;
          }
          checks++;
        }
      }
    }
  }
}

// The modal box. Two properties carry it: it never leaves the screen, and when
// the content does not fit it SHRINKS and says so — because a box that reported
// an inner width it did not have would let the confirmation question and the
// name of the note being deleted print straight through the border.
static void testPopupBox() {
  printf("popup: the modal box stays on the screen\n");

  // Both orientations, and both a comfortable request and absurd ones.
  const int screens[2][2] = {{800, 480}, {480, 800}};
  for (const auto& sc : screens) {
    const int sw = sc[0], sh = sc[1];
    for (int wantW = 0; wantW <= sw + 200; wantW += 37) {
      for (int wantH = 0; wantH <= sh + 200; wantH += 29) {
        const PopupBox b = popupBoxOf(sw, sh, wantW, wantH);

        const bool onScreen = b.x >= 0 && b.y >= 0 &&
                              b.x + b.w <= sw && b.y + b.h <= sh;
        // The margin is what keeps the box clear of the screen's own border,
        // which it is drawn on top of.
        const bool margins = b.x >= POPUP_MARGIN && b.y >= POPUP_MARGIN;
        const bool centred = (b.x - (sw - b.w) / 2) == 0 &&
                             (b.y - (sh - b.h) / 2) == 0;
        const bool innerInside =
            b.innerX >= b.x + POPUP_BORDER && b.innerY >= b.y + POPUP_BORDER &&
            b.innerX + b.innerW <= b.x + b.w - POPUP_BORDER &&
            b.innerY + b.innerH <= b.y + b.h - POPUP_BORDER;
        // Never claims more room than was asked for, and never more than it has.
        const bool honest = b.innerW <= wantW && b.innerH <= wantH &&
                            b.innerW >= 0 && b.innerH >= 0;

        if (!(onScreen && margins && centred && innerInside && honest)) {
          printf("  FAIL  %dx%d want %dx%d -> box %d,%d %dx%d inner %d,%d %dx%d\n",
                 sw, sh, wantW, wantH, b.x, b.y, b.w, b.h,
                 b.innerX, b.innerY, b.innerW, b.innerH);
          failures++;
        }
        checks++;
      }
    }
  }

  // A request that fits is granted exactly — the box is only allowed to shrink
  // when the screen forces it.
  const PopupBox fits = popupBoxOf(800, 480, 300, 120);
  check(fits.innerW == 300, "an inner width that fits is granted");
  check(fits.innerH == 120, "and so is the height");

  // And one that does not is capped at the screen minus its margins.
  const PopupBox huge = popupBoxOf(800, 480, 5000, 5000);
  check(huge.w == 800 - 2 * POPUP_MARGIN, "an oversized box is capped in width");
  check(huge.h == 480 - 2 * POPUP_MARGIN, "and in height");
  check(huge.innerW < 5000 && huge.innerH < 5000,
        "and reports the room it really has, not the room asked for");
}

int main() {
  printf("\n=== ui layout tests ===\n\n");

  testInkFromMetrics();
  testRealFonts();
  testBandGeometry();
  testTextSitsInsideItsBand();
  testFrameGeometry();
  testDensitySurvivesTheScale();
  testFooterStacking();
  testFrameFitsTheScreen();
  testScrollWindow();
  testScrollThumb();
  testPopupBox();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
