#include "ui_layout.h"

TextInk inkFromMetrics(const int ascender, const int descender,
                       const bool haveTall, const int tallTop,
                       const bool haveLow, const int lowHeight, const int lowTop) {
  TextInk ink;
  // glyph->top is the rise of the glyph bitmap above the baseline, and the
  // baseline sits `ascender` below drawText()'s y. So the first inked row of
  // the tallest character is ascender - top.
  ink.top = haveTall ? ascender - tallTop : 0;
  // How far a descender hangs below the baseline. Without a 'g' to measure,
  // fall back on the font's own descender, which is negative.
  ink.bottom = ascender + (haveLow ? lowHeight - lowTop : -descender);
  return ink;
}

int bandHeightOf(const TextInk& ink) { return ink.height() + 2 * ROW_PAD; }

int rowPitchOf(const TextInk& ink) { return bandHeightOf(ink) + ROW_GAP; }

int lineStepOf(const TextInk& ink) { return ink.height() + LINE_LEAD; }

int textYInBandOf(const TextInk& ink, const int bandTop, const int bandH) {
  int y = bandTop + (bandH - ink.height()) / 2 - ink.top;
  if (bandH >= ink.height()) {
    // The band can hold the text: keep every pixel of ink inside it.
    //
    // Neither clamp can actually fire while the centring above is what it is:
    // y - yMin is (bandH - height)/2, which the guard makes non-negative, and
    // yMax - y is ceil((bandH - height)/2), likewise. Brute-forced over 34M
    // combinations of height, ink.top, bandH and bandTop — zero hits. So both
    // survive mutation as equivalent mutants, and that is expected, not a gap
    // in the tests.
    //
    // Kept anyway: it is the centring formula that makes them dead, and the
    // formula is exactly the thing a future layout change would touch.
    const int yMin = bandTop - ink.top;
    const int yMax = bandTop + bandH - ink.bottom;
    if (y < yMin) y = yMin;
    if (y > yMax) y = yMax;
  }
  return y;
}

// ---------------------------------------------------------------------------
// Screen frame
// ---------------------------------------------------------------------------

int contentXOf() { return WORKSPACE_PAD; }

int contentWOf(const int screenW) {
  const int w = screenW - 2 * WORKSPACE_PAD;
  return w > 0 ? w : 0;
}

HeaderLayout headerLayoutOf(const TextInk& titleInk) {
  HeaderLayout h;
  h.boxTop = FRAME_INSET + FRAME_BORDER + HEADER_PAD_Y;
  h.boxH = titleInk.height();
  h.height = HEADER_PAD_Y + h.boxH + HEADER_PAD_Y + HEADER_RULE;
  h.ruleY = FRAME_INSET + FRAME_BORDER + h.height - HEADER_RULE;
  return h;
}

FooterLayout footerLayoutOf(const int screenH, const TextInk& ink, const int contentW,
                            const int wLeft, const int wCentreWide, const int wRight,
                            const bool centreCanStack) {
  FooterLayout f;

  // Three columns laid out space-between need at least one gap between each
  // neighbouring pair. When the wide middle stops fitting, it stacks — and the
  // stacked form is narrower, which is the whole point of stacking it.
  const int wideRun = wLeft + wCentreWide + wRight + 2 * FOOTER_COL_GAP;
  f.rows = (centreCanStack && wCentreWide > 0 && wideRun > contentW) ? 2 : 1;

  f.boxH = f.rows * ink.height() + (f.rows - 1) * LINE_LEAD;
  f.height = FOOTER_RULE + FOOTER_PAD_Y + f.boxH + FOOTER_PAD_Y;
  f.ruleY = screenH - FRAME_INSET - FRAME_BORDER - f.height;
  f.boxTop = f.ruleY + FOOTER_RULE + FOOTER_PAD_Y;
  return f;
}

int bodyTopOf(const HeaderLayout& header) {
  return FRAME_INSET + FRAME_BORDER + header.height;
}

int bodyBottomOf(const FooterLayout& footer) { return footer.ruleY; }

// ---------------------------------------------------------------------------
// Scrolling lists
// ---------------------------------------------------------------------------

int scrollStartOf(const int total, const int visible, const int selected) {
  if (visible <= 0 || total <= visible) return 0;

  // Keep the selection inside the window, scrolling by the least that achieves
  // it: moving down parks it on the last row, moving up on the first.
  int start = selected - visible + 1;
  if (start < 0) start = 0;

  // The next two lines cannot fire for an in-range `selected` — brute-forced
  // over 1.1M shapes, zero hits — so both survive mutation as equivalent
  // mutants. They are kept as the guard against an out-of-range selection,
  // which is a caller bug this should absorb rather than turn into a negative
  // index or a window hanging off the end of the list.
  if (selected < start) start = selected;
  const int maxStart = total - visible;
  if (start > maxStart) start = maxStart;
  return start;
}

ScrollThumb scrollThumbOf(const int trackH, const int total, const int visible,
                          const int start) {
  ScrollThumb t{0, 0};
  if (trackH <= 0 || visible <= 0 || total <= visible) return t;   // nothing to show

  t.h = trackH * visible / total;
  if (t.h < SCROLLBAR_MIN_THUMB) t.h = SCROLLBAR_MIN_THUMB;
  if (t.h > trackH) t.h = trackH;

  // Travel is the track minus the thumb, so the thumb's bottom lands exactly on
  // the track's bottom at the end of the list.
  const int maxStart = total - visible;
  t.y = maxStart > 0 ? (trackH - t.h) * start / maxStart : 0;
  if (t.y < 0) t.y = 0;
  // Also an equivalent mutant, and for the same reason as the pair in
  // scrollStartOf: with the travel computed above, y + h never exceeds trackH.
  if (t.y + t.h > trackH) t.y = trackH - t.h;
  return t;
}

PopupBox popupBoxOf(const int screenW, const int screenH,
                    const int innerW, const int innerH) {
  const int chromeX = 2 * (POPUP_BORDER + POPUP_PAD_X);
  const int chromeY = 2 * (POPUP_BORDER + POPUP_PAD_Y);

  PopupBox b{};
  b.w = (innerW > 0 ? innerW : 0) + chromeX;
  b.h = (innerH > 0 ? innerH : 0) + chromeY;

  const int maxW = screenW - 2 * POPUP_MARGIN;
  const int maxH = screenH - 2 * POPUP_MARGIN;
  if (b.w > maxW) b.w = maxW;
  if (b.h > maxH) b.h = maxH;
  // A screen too small to hold the chrome is not a case this device has, but
  // the arithmetic below must not hand back a negative width if it ever is.
  if (b.w < 0) b.w = 0;
  if (b.h < 0) b.h = 0;

  b.x = (screenW - b.w) / 2;
  b.y = (screenH - b.h) / 2;
  if (b.x < 0) b.x = 0;
  if (b.y < 0) b.y = 0;

  b.innerX = b.x + POPUP_BORDER + POPUP_PAD_X;
  b.innerY = b.y + POPUP_BORDER + POPUP_PAD_Y;
  b.innerW = b.w - chromeX;
  b.innerH = b.h - chromeY;
  if (b.innerW < 0) b.innerW = 0;
  if (b.innerH < 0) b.innerH = 0;
  return b;
}
