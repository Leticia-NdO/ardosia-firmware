// Copied from epdiy — https://github.com/vroland/epdiy — specifically
// src/epd_internals.h at commit c61e9e923ce2418150d54f88cea5d196cdc40c54.
//
// SPDX-License-Identifier: LGPL-3.0-or-later
//
// THIS FILE IS NOT MIT. The rest of this repository is (see ../../LICENSE);
// this file keeps the licence it came with, and epdiy's root LICENSE at the
// commit named above is the GNU Lesser General Public License version 3 —
// verified against that exact commit, not merely against epdiy's current HEAD.
// The full text is in ../../LICENSES/LGPL-3.0.txt, and the GPL-3.0 text that
// LGPL-3.0 incorporates by reference is in ../../LICENSES/GPL-3.0.txt.
//
// What is here is a data-structure layout: the four structs below describe how
// a converted font is laid out in flash, and the bitmap headers in
// builtinFonts/ are generated against them by scripts/fontconvert.py. LGPL-3.0
// section 3 permits combined works incorporating material of this kind subject
// to notice, which is what this header and the repository's NOTICE are.
//
// Copyright (c) the epdiy authors.

#pragma once
#include <cstdint>

/// Font data stored PER GLYPH
typedef struct {
  uint8_t width;        ///< Bitmap dimensions in pixels
  uint8_t height;       ///< Bitmap dimensions in pixels
  uint8_t advanceX;     ///< Distance to advance cursor (x axis)
  int16_t left;         ///< X dist from cursor pos to UL corner
  int16_t top;          ///< Y dist from cursor pos to UL corner
  uint16_t dataLength;  ///< Size of the font data.
  uint32_t dataOffset;  ///< Pointer into EpdFont->bitmap
} EpdGlyph;

/// Glyph interval structure
typedef struct {
  uint32_t first;   ///< The first unicode code point of the interval
  uint32_t last;    ///< The last unicode code point of the interval
  uint32_t offset;  ///< Index of the first code point into the glyph array
} EpdUnicodeInterval;

/// Data stored for FONT AS A WHOLE
typedef struct {
  const uint8_t* bitmap;                ///< Glyph bitmaps, concatenated
  const EpdGlyph* glyph;                ///< Glyph array
  const EpdUnicodeInterval* intervals;  ///< Valid unicode intervals for this font
  uint32_t intervalCount;               ///< Number of unicode intervals.
  uint8_t advanceY;                     ///< Newline distance (y axis)
  int ascender;                         ///< Maximal height of a glyph above the base line
  int descender;                        ///< Maximal height of a glyph below the base line
  bool is2Bit;
} EpdFontData;
