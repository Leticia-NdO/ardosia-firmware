#pragma once

// -----------------------------------------------------------------------------
// BoardConfig shim — MicroSlate fork (Xteink X4, ESP32-C3)
// -----------------------------------------------------------------------------
// RecoveryBoot.cpp comes verbatim from the FreeInk SDK, where it calls two
// BoardConfig helpers before an SD flash. MicroSlate does not vendor
// BoardConfig, so this shim supplies exactly those two symbols and nothing
// else. Keeping the shim here (instead of patching RecoveryBoot.cpp) means the
// SDK sources stay byte-identical to upstream and rebase cleanly.
//
// Why no-ops are correct on this hardware:
//
//   holdPowerRails() asserts power.latch0 / power.latch1 on battery-latched
//   boards, so releasing the power button mid-flash can't cut power. The
//   Xteink X4 is not battery-latched — MicroSlate runs for hours today without
//   ever calling it.
//
//   releaseSdRail() rescues a switched SD power rail that a previous sleep path
//   may have latched off. On the X4 the card is brought up by MicroSlate's own
//   SDCardManager::begin(), which is called before any flash attempt.
//
// VERIFY BEFORE TRUSTING: if a flash from SD ever dies partway with the power
// button released, the latch assumption is wrong — implement holdPowerRails()
// against the real pins instead of leaving it empty.
// -----------------------------------------------------------------------------

namespace BoardConfig {

inline void holdPowerRails() {}
inline void releaseSdRail() {}

}  // namespace BoardConfig
