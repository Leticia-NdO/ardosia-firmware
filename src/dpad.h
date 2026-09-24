#pragma once

// Which way the four direction buttons point once the screen has been rotated.
// Free of Arduino so test/run.sh can exercise it: a rotation table is four
// lines of lookup and a whole afternoon of pressing buttons on a device that
// needs an SD card write per attempt.
//
// The problem it replaces: every list screen used to read
//
//     if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast))  -> move up
//
// The `|| btnRight` is a landscape patch — with the screen turned, the button
// on the right of the case is the one that points up the page. It works in one
// landscape and is wrong in the other, it was copied into six blocks, and it
// leaves Left and Right doing double duty everywhere.

#include "config.h"
#include <cstdint>

enum class Dir : uint8_t { Up = 0, Down, Left, Right, Count };

// Fixed  — the buttons mean what they are labelled, whatever the screen does.
//          This is what the firmware has always done, patch included, and it
//          stays the default: it is what the muscle memory expects.
// Natural — the buttons mean what they point at on the rotated screen.
enum class DpadMode : uint8_t { Fixed = 0, Natural = 1 };

// The logical direction a physical button points.
//
// In Fixed mode this is the identity. In Natural mode it is the screen
// rotation, and it is a bijection: every button lands on exactly one
// direction and every direction is reachable.
//
// The two landscape cases were exchanged on 2026-09-23 after trying them on the
// device: the table had been built from the old `|| btnRight` patch, which
// describes the case the upstream author held, and Ardosia is held the other
// way round — bottom edge to the RIGHT, side keys above. The mapping came out
// reversed, and exchanging the two cases in dpad.cpp is the whole fix.
//
// Which way GfxRenderer's two landscape values turn the panel is still not
// something this code can derive, so the tests pin the table that the device
// agreed with rather than a rotation this file worked out for itself.
Dir dpadResolve(Dir physical, Orientation orientation, DpadMode mode);
