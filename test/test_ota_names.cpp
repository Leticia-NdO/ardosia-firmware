// What another OTA slot is called in the main menu (src/ota_names.cpp).
//
// The bug this file exists for, observed on the device: ota_0 had held CrossInk,
// the Escape Hatch was written over it from Settings, and the main menu went on
// offering "CrossInk". Choosing it booted the hatch correctly — only the label
// lied, and it kept lying across a reboot.
//
// Three separate defects produced that one symptom, and only the first is in
// this file:
//   1. the nickname in NVS was keyed by SLOT and nothing tied it to the slot's
//      contents, so it outlived the firmware it described;
//   2. detectOtaApps() fetched the slot's esp_app_desc_t and threw everything
//      away but "did it parse", so there was no truth to fall back to;
//   3. detectOtaApps() ran once in setup(), so an install in this session was
//      invisible until a reboot.
//
// The property that matters here is the one the old code got wrong: a nickname
// is shown ONLY when it can be shown to still describe what is in the slot.

#include <cstdio>
#include <cstring>

#include "ota_names.h"

static int failures = 0;

static void check(const bool ok, const char* what) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static void checkName(const OtaNameInputs& in, const char* want, const char* what) {
  char got[32];
  otaDisplayName(in, got, sizeof(got));
  if (std::strcmp(got, want) == 0) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n        got  \"%s\"\n        want \"%s\"\n", what, got, want);
  }
}

static void testTheBugItself() {
  printf("the stale nickname, which is the bug\n");

  // Exactly the device's state: the slot now holds the hatch, NVS still says
  // CrossInk, and the recorded project no longer matches.
  OtaNameInputs stale;
  stale.nickname = "CrossInk";
  stale.nicknameProject = "crossink";
  stale.slotProject = "arduino-lib-builder";
  stale.slotIsEmbeddedHatch = true;
  stale.slot = 0;
  checkName(stale, "Recovery",
            "a nickname whose project no longer matches is dropped");

  // And the same without the hatch recognition, to prove the nickname is
  // dropped on its own and not merely outranked.
  OtaNameInputs staleNoHatch = stale;
  staleNoHatch.slotProject = "something-else";
  staleNoHatch.slotIsEmbeddedHatch = false;
  checkName(staleNoHatch, "something-else",
            "...falling through to what the slot says it is");
}

static void testNicknameIsKeptWhenItStillApplies() {
  printf("a nickname that still describes the slot survives\n");

  // This is the behaviour renameOtaApp() exists for and must not lose: a name
  // the user chose has to survive an UPDATE of the same app. Only a
  // REPLACEMENT by a different app invalidates it.
  OtaNameInputs in;
  in.nickname = "My Reader";
  in.nicknameProject = "crosspoint";
  in.slotProject = "crosspoint";
  in.slot = 1;
  checkName(in, "My Reader", "same project -> the chosen name is kept");

  // Even when the slot happens to hold our hatch build, an explicit name wins:
  // the user said what to call it.
  OtaNameInputs named;
  named.nickname = "Rescue";
  named.nicknameProject = "arduino-lib-builder";
  named.slotProject = "arduino-lib-builder";
  named.slotIsEmbeddedHatch = true;
  checkName(named, "Rescue", "an explicit name outranks the built-in label");
}

static void testUnverifiableNicknameIsNotTrusted() {
  printf("a nickname with no recorded project is not trusted\n");

  // Written before the project was tracked, so it cannot be checked. Treating
  // it as valid is exactly the old behaviour and the old bug; treating it as
  // stale makes the display self-healing on the first boot after the fix, at
  // the cost of one-time loss of hand-set names.
  OtaNameInputs legacy;
  legacy.nickname = "CrossInk";
  legacy.nicknameProject = "";
  legacy.slotProject = "crosspoint";
  legacy.slot = 0;
  checkName(legacy, "crosspoint", "an unverifiable nickname loses to the descriptor");

  // The case that makes the explicit emptiness test load-bearing, found by
  // mutation: when the slot's project name is ALSO empty, strcmp("", "") agrees
  // and a comparison alone would trust the nickname. Only checking that a
  // project was actually recorded separates "verified equal" from "both blank".
  //
  // Reachable: a firmware built without a project name set, with a nickname in
  // NVS from before the project was tracked.
  OtaNameInputs bothBlank;
  bothBlank.nickname = "CrossInk";
  bothBlank.nicknameProject = "";
  bothBlank.slotProject = "";
  bothBlank.slot = 2;
  checkName(bothBlank, "OTA Slot 2",
            "two blank projects are not a match — the nickname is still dropped");
}

static void testFallbackChain() {
  printf("the fallback chain, in order\n");

  OtaNameInputs hatch;
  hatch.slotProject = "arduino-lib-builder";
  hatch.slotIsEmbeddedHatch = true;
  hatch.slot = 1;
  checkName(hatch, "Recovery",
            "our own recovery build is named, not left as arduino-lib-builder");

  OtaNameInputs project;
  project.slotProject = "crosspoint";
  project.slot = 1;
  checkName(project, "crosspoint", "otherwise the slot's own project name");

  OtaNameInputs nothing;
  nothing.slot = 3;
  checkName(nothing, "OTA Slot 3", "and with nothing to go on, the slot number");

  // A default-constructed input must not produce an empty label: a blank menu
  // row on a device with no serial console is indistinguishable from a bug.
  char buf[32];
  const OtaNameInputs fresh;
  otaDisplayName(fresh, buf, sizeof(buf));
  check(buf[0] != '\0', "a default-constructed input still produces a label");
}

static void testTrimming() {
  printf("the padding esp_app_desc_t carries is trimmed\n");

  // project_name and version are fixed-width char arrays, space- and NUL-padded
  // by the build. Untrimmed they make a menu row read "crosspoint          ".
  char out[64];
  otaTrimField("crosspoint          ", out, sizeof(out));
  check(std::strcmp(out, "crosspoint") == 0, "trailing spaces go");

  otaTrimField("", out, sizeof(out));
  check(out[0] == '\0', "an empty field stays empty");

  otaTrimField(nullptr, out, sizeof(out));
  check(out[0] == '\0', "a null field is not dereferenced");

  otaTrimField("    ", out, sizeof(out));
  check(out[0] == '\0', "all-spaces trims to empty, so the chain falls through");

  // And trimming must not change a comparison's answer: a padded recorded
  // project has to still match an unpadded live one.
  OtaNameInputs padded;
  padded.nickname = "My Reader";
  padded.nicknameProject = "crosspoint   ";
  padded.slotProject = "crosspoint";
  checkName(padded, "My Reader", "padding does not break the project match");
}

static void testBufferSafety() {
  printf("a short buffer truncates instead of overrunning\n");

  // ASan is watching. OtaAppEntry::name is 32 bytes, but nothing stops a caller
  // passing less.
  OtaNameInputs in;
  in.nickname = "A very long nickname indeed, far past the field";
  in.nicknameProject = "p";
  in.slotProject = "p";
  char tiny[8];
  otaDisplayName(in, tiny, sizeof(tiny));
  check(std::strlen(tiny) < sizeof(tiny), "truncated, still NUL-terminated");

  char one[1];
  otaDisplayName(in, one, sizeof(one));
  check(one[0] == '\0', "a one-byte buffer gets just the terminator");
}

int main() {
  printf("\n=== OTA slot names ===\n\n");
  testTheBugItself();
  printf("\n");
  testNicknameIsKeptWhenItStillApplies();
  printf("\n");
  testUnverifiableNicknameIsNotTrusted();
  printf("\n");
  testFallbackChain();
  printf("\n");
  testTrimming();
  printf("\n");
  testBufferSafety();
  printf("\n");

  if (failures) {
    printf("FAILURES: %d\n", failures);
    return 1;
  }
  printf("OTA slot names: every check passed\n");
  return 0;
}
