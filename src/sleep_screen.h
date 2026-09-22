#pragma once

#include "config.h"

class GfxRenderer;

// Draw a wallpaper into the framebuffer for the sleep screen.
//
// Does NOT refresh the panel — the caller owns that, because entering deep
// sleep needs exactly one FULL_REFRESH after everything is composed.
//
// Returns false when no usable image was found (no card, no /sleep directory,
// no valid BMP). The caller MUST fall back to the built-in text screen: this
// device has no USB recovery, so "asleep showing nothing" is a state worth
// never creating.
//
// On success the panel has already been refreshed (4-level grayscale for
// photos, a single half refresh for 1-bit art). The caller must not refresh
// again.
bool sleepScreenDrawImage(GfxRenderer& renderer, SleepScreenMode mode);
