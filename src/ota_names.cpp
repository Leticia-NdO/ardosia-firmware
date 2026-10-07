#include "ota_names.h"

#include <stdio.h>
#include <string.h>

void otaTrimField(const char* src, char* out, const size_t n) {
  if (!out || n == 0) return;
  out[0] = '\0';
  if (!src) return;

  size_t len = strnlen(src, n - 1);
  while (len > 0 && (src[len - 1] == ' ' || src[len - 1] == '\t')) len--;
  memcpy(out, src, len);
  out[len] = '\0';
}

void otaDisplayName(const OtaNameInputs& in, char* out, const size_t n) {
  if (!out || n == 0) return;

  char nickname[64];
  char nickProject[64];
  char slotProject[64];
  otaTrimField(in.nickname, nickname, sizeof(nickname));
  otaTrimField(in.nicknameProject, nickProject, sizeof(nickProject));
  otaTrimField(in.slotProject, slotProject, sizeof(slotProject));

  // 1. A nickname that can be shown to still belong to what is in the slot.
  //    Requires a recorded project that matches: an unverifiable nickname is
  //    exactly the stale-label bug this function exists to fix.
  if (nickname[0] != '\0' && nickProject[0] != '\0' &&
      strcmp(nickProject, slotProject) == 0) {
    snprintf(out, n, "%s", nickname);
    return;
  }

  // 2. Our own embedded recovery firmware, which we can identify exactly. Named
  //    here rather than left to its project_name because the Arduino core sets
  //    that to "arduino-lib-builder", which identifies nothing.
  if (in.slotIsEmbeddedHatch) {
    snprintf(out, n, "Recovery");
    return;
  }

  // 3. Whatever the slot says it is. Not pretty for every build, but true.
  if (slotProject[0] != '\0') {
    snprintf(out, n, "%s", slotProject);
    return;
  }

  // 4. Nothing to go on.
  snprintf(out, n, "OTA Slot %d", in.slot);
}
