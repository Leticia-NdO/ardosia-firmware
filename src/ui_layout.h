#pragma once

// The vertical arithmetic behind every screen, with no renderer and no font
// blob in it, so test/run.sh can exercise it on a host compiler. Same split as
// sleep_layout and keymap/deadkeys: on a device with no serial console, logic
// that cannot be tested off-device cannot be tested at all.
//
// WHY THIS EXISTS AT ALL — the bug it encodes the fix for:
//
// GfxRenderer::drawText(font, x, y, ...) treats `y` as the top of the font's
// ASCENDER BOX and puts the baseline at y + ascender. That ascender is the
// tallest glyph anywhere in the font (27px in notosans_12, 28 in
// jetbrainsmono_13), while ordinary text only reaches cap height. So a label
// drawn at the top of a highlight band sits ~9px lower than it looks like it
// should, and its descenders land past the bottom of the band. Since the
// selected row draws its text in the inverted colour, whatever leaves the band
// simply disappears — that is the "words sit low and get clipped along the
// bottom" symptom that hit every menu.
//
// The fix is to measure the band the text actually paints and centre THAT.

#include "config.h"

// The vertical extent a line of text actually inks, measured downward from the
// `y` handed to drawText(). `top` is the first inked row, `bottom` is one past
// the last.
struct TextInk {
  int top;
  int bottom;
  int height() const { return bottom - top; }
};

// Build a TextInk out of raw font-table numbers, so the arithmetic is reachable
// without a font blob. The caller looks the glyphs up; this decides what they
// mean.
//
//   ascender / descender  EpdFontData fields (descender is negative)
//   tall*                 the glyph for 'Á', or 'A' when the font lacks Á
//   low*                  the glyph for 'g'
//
// Á rather than A on purpose: a layout calibrated on unaccented ASCII clips
// every "á" and "ç" the day pt-br text lands in it.
TextInk inkFromMetrics(int ascender, int descender,
                       bool haveTall, int tallTop,
                       bool haveLow, int lowHeight, int lowTop);

// A selectable row = the highlight band, plus the gap to the next row.
//
// THE ONE PLACE THE DESIGN IS NOT FOLLOWED LITERALLY, and the reason:
//
// The Figma draws a 48px row around 15px text — about 60% of the row is
// padding. Drawn 1:1 that was unreadable on this panel, and scaling the whole
// thing uniformly is self-defeating: at the text size the panel actually needs
// (31px of ink), a proportional 26px of padding makes an 86px row, and a
// landscape screen fits two of them. Measured, not guessed.
//
// So the text scales ~1.8x from the Figma and the padding does not. Rows stay
// dense, the style survives, the airiness is what gets spent.
//
//                     Figma    here
//   row padding        ~14       8
//   row gap              0       0     rows are flush, split by a rule
//   header padding Y    12      10
//   workspace padding   20      12
//   footer padding Y    10      10     unchanged
//   border and rules     2       2     unchanged
static constexpr int ROW_PAD = 8;   // clear space above/below the ink in a band
static constexpr int ROW_GAP = 0;   // rows sit flush; a 1px rule separates them

// Extra leading between plain stacked lines of text.
static constexpr int LINE_LEAD = 3;

// ---------------------------------------------------------------------------
// Screen frame, from prototypes/*.css
//
// The Figma canvas is the panel's own resolution (800x480 landscape, 480x800
// portrait), so every number here is a device pixel, not a CSS abstraction.
// Comparing the two exports shows the portrait layout is the same design
// rescaled — identical paddings and row heights, only the content column
// narrows — which is why one set of numbers drives all four orientations.
// ---------------------------------------------------------------------------

static constexpr int FRAME_BORDER = 2;    // border drawn around the whole screen

// How far in from the panel edge that border sits. GfxRenderer declares
// VIEWABLE_MARGIN_* (top 9, sides 3) for a bezel that covers the outermost
// pixels, but nothing in this firmware or in lib/ ever calls
// getOrientedViewableTRBL() to honour them, and the current UI has been drawing
// headers at y=5 legibly all along. So the border goes right up to the edge,
// as the design draws it. If it turns out the bezel eats it on some side, this
// is the one number to raise.
static constexpr int FRAME_INSET = 0;

static constexpr int HEADER_PAD_X = 24;
static constexpr int HEADER_PAD_Y = 10;   // Figma 12; see the note on ROW_PAD
static constexpr int HEADER_RULE  = 2;

static constexpr int WORKSPACE_PAD = 12;  // Figma 20

// Separator drawn under an unselected row. The selected one is a filled band,
// so it needs none.
static constexpr int ROW_RULE = 1;

// Corner radius of the selected row's band. The Figma's 4px, unscaled: a corner
// is a corner, and at 8px it would start reading as a pill.
static constexpr int ROW_RADIUS = 4;

static constexpr int FOOTER_PAD_X = 24;
static constexpr int FOOTER_PAD_Y = 10;
static constexpr int FOOTER_RULE  = 2;

// Smallest space allowed between two footer columns before the middle one
// gives up its single line and stacks.
static constexpr int FOOTER_COL_GAP = 16;

// The body column: x and width of everything between the side paddings.
// 800 - 40 = 760 and 480 - 40 = 440, which is what both CSS exports use.
int contentXOf();
int contentWOf(int screenW);

// The header band: a title on the left, status on the right, a rule under both.
struct HeaderLayout {
  int height;   // total, rule included
  int ruleY;    // top row of the rule
  int boxTop;   // top of the content box, for textYInBandOf
  int boxH;     // height of that box
};
HeaderLayout headerLayoutOf(const TextInk& titleInk);

// The footer band: up to three columns, space-between, vertically centred.
// Only the middle column ever reflows — the design keeps the outer two on one
// line at every width and stacks the shortcut pair in the middle when the three
// stop fitting side by side. `wCentreWide` is the one-line form — the one with
// the "|" separator. The stacked form's width is not needed: it is narrower by
// construction, and if even that overflows there is nothing left to do but let
// truncation handle it.
struct FooterLayout {
  int rows;     // rows in the middle column: 1 or 2
  int height;   // total, rule included
  int ruleY;    // top row of the rule
  int boxTop;   // top of the content box
  int boxH;     // height of that box
};
// `centreCanStack` says whether the middle column has a stacked form at all.
// A screen whose middle column is a single indivisible string (the Settings key
// readout, say) passes false and keeps one row however tight it gets, letting
// truncation do the work instead of blanking the line.
FooterLayout footerLayoutOf(int screenH, const TextInk& ink, int contentW,
                            int wLeft, int wCentreWide, int wRight,
                            bool centreCanStack);

// First and last usable y of the workspace between header and footer.
int bodyTopOf(const HeaderLayout& header);
int bodyBottomOf(const FooterLayout& footer);

// ---------------------------------------------------------------------------
// Scrolling lists
// ---------------------------------------------------------------------------

// Width of the scrollbar track drawn down the right edge of a list, plus the
// clear space between it and the row text. Nothing in the prototypes covers a
// list longer than its screen, but Settings has nine rows and a landscape
// screen holds six, so the overflow has to be visible somehow. A track costs no
// vertical space, which a "5 of 9" line would.
static constexpr int SCROLLBAR_W = 4;
static constexpr int SCROLLBAR_GAP = 6;
static constexpr int SCROLLBAR_MIN_THUMB = 12;

// First index of the visible window, chosen so `selected` is inside it and the
// window never runs off either end. A list that fits returns 0.
int scrollStartOf(int total, int visible, int selected);

// Where the thumb goes inside a track of `trackH` pixels. Height is the
// visible fraction, position the scrolled fraction. Returns h == 0 when the
// whole list fits and no track should be drawn at all.
struct ScrollThumb {
  int y;
  int h;
};
ScrollThumb scrollThumbOf(int trackH, int total, int visible, int start);

int bandHeightOf(const TextInk& ink);
int rowPitchOf(const TextInk& ink);

// Distance between successive lines of plain (unbanded) stacked text.
// NOT the font's advanceY: for ubuntu_10 that equalled the ink height exactly,
// so stacked lines came out touching.
int lineStepOf(const TextInk& ink);

// The `y` to hand drawText() so its ink is centred in [bandTop, bandTop+bandH).
// When the band can hold the text, the result is clamped so no inked pixel
// falls outside the band.
int textYInBandOf(const TextInk& ink, int bandTop, int bandH);

// ---------------------------------------------------------------------------
// Modal box (the confirmation dialog)
// ---------------------------------------------------------------------------
//
// Not in the prototypes — the design has no modal — so the numbers come from
// the frame it sits on top of: the same 2px border as the screen, the footer's
// 24px side padding, and a margin that keeps it clear of the border it covers.
static constexpr int POPUP_BORDER = 2;
static constexpr int POPUP_PAD_X  = 24;
static constexpr int POPUP_PAD_Y  = 16;
static constexpr int POPUP_MARGIN = 24;   // least clear space to the screen edge
static constexpr int POPUP_GAP    = 16;   // between the question and the buttons
static constexpr int POPUP_BTN_PAD_X = 16;
static constexpr int POPUP_BTN_GAP   = 12;

// The Settings tab bar. A tab is drawn as the same pill as a dialog button but
// wants more room than one: it is the first thing on the screen and carries two
// words, and at the button's padding the labels read as cramped against the
// sides. The tight pair is the fallback tier for portrait, where three tabs and
// 440px leave no choice — see the measured table in drawSettingsMenu().
static constexpr int TAB_PAD_X = 28;
static constexpr int TAB_GAP   = 20;
static constexpr int TAB_PAD_X_TIGHT = 10;
static constexpr int TAB_GAP_TIGHT   = 12;

struct PopupBox {
  int x, y, w, h;          // the outer box, border included
  int innerX, innerY;      // first drawable pixel inside border and padding
  int innerW, innerH;      // room actually left, which may be less than asked
};

// A box of the requested inner size, centred on the screen and never off it.
// When the request does not fit, the box is capped at the screen minus its
// margins and `innerW`/`innerH` report what is really available, so the caller
// truncates its text instead of drawing outside the border.
PopupBox popupBoxOf(int screenW, int screenH, int innerW, int innerH);
