#!/bin/bash
#
# Regenerates the bitmap headers in ../builtinFonts from the TTFs in
# ../builtinFonts/source. Run it from anywhere; it cd's to its own directory.
#
# EVERY FONT LISTED HERE IS COMPILED INTO THE FIRMWARE, and every header in
# ../builtinFonts is produced by a line below. That is a deliberate property,
# not a coincidence: this repository used to carry four typefaces and sixty-odd
# converted headers that nothing included, and each one was a font licence to
# honour in exchange for no pixels on the panel. One of them was a commercial
# e-reader face with no licence permitting redistribution at all, which made the
# repository as a whole undistributable — that one is why this note exists.
#
# So: do not add a font here that src/ui_renderer.cpp does not include, and do
# not leave a header in ../builtinFonts that no line here produces.
#
# The two faces that remain are both SIL Open Font License 1.1, with their
# OFL.txt kept beside the TTFs in source/ and listed in the repository's NOTICE.

set -e

cd "$(dirname "$0")"

# --- Noto Sans: the EDITOR's body text ------------------------------------
# Regular and bold only. The editor never draws an italic span, and
# EpdFontFamily::getFont() falls back to regular for a null slot — so italics
# were flash spent on nothing. Sizes map onto Settings -> Editor -> Font Size
# (Small/Medium/Large) via editorFontId() in src/config.h.
NOTOSANS_SIZES=(12 14 16)
NOTOSANS_STYLES=("Regular" "Bold")

for size in "${NOTOSANS_SIZES[@]}"; do
  for style in "${NOTOSANS_STYLES[@]}"; do
    font_name="notosans_${size}_$(echo "$style" | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py "$font_name" "$size" "$font_path" --2bit > "$output_path"
    echo "Generated $output_path"
  done
done

# --- JetBrains Mono: the CHROME, and the editor's Mono option --------------
# Two groups with different style sets, so they cannot share one loop:
#
#   9 11 13 18   chrome — menus, headers, footers, lists. Regular AND bold;
#                the selected row and the screen title are bold.
#   12 14 16     the editor's Mono faces, matching the Noto Sans point sizes
#                above so Font Size means the same thing in either typeface.
#                Regular only — the editor draws no styled span.
#
# SIZES ARE IN POINTS AT 150 DPI, not pixels: fontconvert.py calls
# set_char_size(size << 6, size << 6, 150, 150), so a 13pt face is 27px of ink.
# See the FONT_CHROME_* block in src/config.h before changing one.
JETBRAINS_CHROME_SIZES=(9 11 13 18)
JETBRAINS_CHROME_STYLES=("Regular" "Bold")

for size in "${JETBRAINS_CHROME_SIZES[@]}"; do
  for style in "${JETBRAINS_CHROME_STYLES[@]}"; do
    font_name="jetbrainsmono_${size}_$(echo "$style" | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/JetBrainsMono/JetBrainsMono-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py "$font_name" "$size" "$font_path" --2bit > "$output_path"
    echo "Generated $output_path"
  done
done

JETBRAINS_EDITOR_SIZES=(12 14 16)

for size in "${JETBRAINS_EDITOR_SIZES[@]}"; do
  font_name="jetbrainsmono_${size}_regular"
  font_path="../builtinFonts/source/JetBrainsMono/JetBrainsMono-Regular.ttf"
  output_path="../builtinFonts/${font_name}.h"
  python fontconvert.py "$font_name" "$size" "$font_path" --2bit > "$output_path"
  echo "Generated $output_path"
done
