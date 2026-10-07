// The embedded recovery firmware: the decision logic, and the blob itself.
//
// Two jobs, and the second is the one that earns its keep:
//
//  1. hatchOfferFor() — what Settings may offer, and in particular what it
//     REFUSES to offer. The interesting properties are negative, because the
//     action overwrites a whole OTA slot on a device whose only recovery route
//     may be the thing in it.
//
//  2. assets/hatch.bin — parsed and integrity-checked as a real ESP32 image.
//     This is what catches a truncated, wrong-chip or half-copied blob at build
//     time rather than on the glass, and it is why the firmware itself needs no
//     runtime SHA code: the payload travels inside an image the bootloader has
//     already hash-verified, so the only corruption that can reach a device is
//     corruption that was there when we built it. This test is that check.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hatch.h"

static int failures = 0;

static void check(const bool ok, const char* what) {
  if (ok) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n", what);
  }
}

static void checkEq(const int got, const int want, const char* what) {
  if (got == want) {
    printf("  ok    %s\n", what);
  } else {
    failures++;
    printf("  FAIL  %s\n        got  %d\n        want %d\n", what, got, want);
  }
}

// --- a synthetic image, so the logic tests do not need the real blob ---------

static std::vector<uint8_t> makeImage(const char* version, const char* project,
                                      uint32_t secureVersion = 0,
                                      uint16_t chipId = kChipIdEsp32C3,
                                      uint8_t magic = kEspImageMagic,
                                      uint32_t descMagic = kAppDescMagic) {
  std::vector<uint8_t> img(kAppDescOff + 16 + 2 * kAppStrLen, 0);
  img[0] = magic;
  img[12] = static_cast<uint8_t>(chipId & 0xFF);
  img[13] = static_cast<uint8_t>(chipId >> 8);
  auto put32 = [&img](size_t off, uint32_t v) {
    img[off] = v & 0xFF;
    img[off + 1] = (v >> 8) & 0xFF;
    img[off + 2] = (v >> 16) & 0xFF;
    img[off + 3] = (v >> 24) & 0xFF;
  };
  put32(kAppDescOff, descMagic);
  put32(kAppDescOff + 4, secureVersion);
  std::memcpy(&img[kAppDescOff + 16], version, std::strlen(version));
  std::memcpy(&img[kAppDescOff + 16 + kAppStrLen], project, std::strlen(project));
  return img;
}

static AppImageInfo infoOf(const std::vector<uint8_t>& img) {
  AppImageInfo info;
  hatchParseImage(img.data(), img.size(), &info);
  return info;
}

// --- parsing ----------------------------------------------------------------

static void testParsing() {
  printf("parsing an app image\n");

  const AppImageInfo good = infoOf(makeImage("8cabf2c", "arduino-lib-builder", 7));
  check(good.valid, "a well-formed image parses");
  checkEq(static_cast<int>(good.secureVersion), 7, "secure_version is read from 0x24");
  checkEq(static_cast<int>(good.chipId), kChipIdEsp32C3, "chip id is read from 0x0C");
  check(std::strncmp(good.version, "8cabf2c", 7) == 0, "version string is read");
  check(std::strncmp(good.projectName, "arduino-lib-builder", 19) == 0,
        "project name is read");

  AppImageInfo info;
  check(!hatchParseImage(nullptr, 1024, &info), "a null buffer is rejected");
  check(!hatchParseImage(makeImage("a", "b").data(), 0, &info),
        "a zero length is rejected");

  // Short prefixes must be refused, not read past. A caller handing over the
  // first sector of a partition is a normal thing to do.
  const std::vector<uint8_t> img = makeImage("a", "b");
  for (size_t len = 0; len < img.size(); len += 7) {
    if (!hatchParseImage(img.data(), len, &info)) continue;
    failures++;
    printf("  FAIL  a %zu-byte prefix parsed when the descriptor needs %zu\n",
           len, img.size());
    break;
  }
  check(true, "every short prefix is refused rather than read past");

  check(!infoOf(makeImage("a", "b", 0, kChipIdEsp32C3, 0x00)).valid,
        "a bad image magic is rejected");
  check(!infoOf(makeImage("a", "b", 0, kChipIdEsp32C3, kEspImageMagic, 0xDEADBEEF)).valid,
        "a bad descriptor magic is rejected");

  // A wrong-chip image still PARSES — it is well-formed, just not ours. The
  // refusal belongs to hatchOfferFor(), which can say why.
  const AppImageInfo wrongChip = infoOf(makeImage("a", "b", 0, 99));
  check(wrongChip.valid, "a wrong-chip image parses (the offer refuses it, not the parser)");
}

static void testSameBuild() {
  printf("telling two builds apart\n");

  const AppImageInfo a = infoOf(makeImage("8cabf2c", "arduino-lib-builder"));
  const AppImageInfo b = infoOf(makeImage("8cabf2c", "arduino-lib-builder"));
  const AppImageInfo other = infoOf(makeImage("9.9.9", "arduino-lib-builder"));
  const AppImageInfo ardosia = infoOf(makeImage("v2.0.3", "xteink-writer-firmware"));

  check(hatchSameBuild(a, b), "identical descriptors are the same build");
  check(!hatchSameBuild(a, other),
        "same project, different version is NOT the same build");
  check(!hatchSameBuild(a, ardosia), "Ardosia is not mistaken for the hatch");

  // The property that matters for idempotence: the hatch's project name is set
  // by the Arduino core, so it is shared with anything else built that way.
  // Version has to be part of the comparison or a different Arduino firmware in
  // the other slot would read as "already installed".
  const AppImageInfo otherArduinoApp = infoOf(makeImage("deadbee", "arduino-lib-builder"));
  check(!hatchSameBuild(a, otherArduinoApp),
        "another Arduino-built firmware is not read as the hatch already installed");

  // And the mirror of it, which mutation found missing: two DIFFERENT projects
  // carrying the SAME version string. Comparing version alone passed every other
  // check here, because every other pair differed in both fields. Two projects
  // both sitting at "1.0.0" is not a contrived case — it is the default state of
  // any two projects that version by hand.
  const AppImageInfo hatchAt100 = infoOf(makeImage("1.0.0", "arduino-lib-builder"));
  const AppImageInfo readerAt100 = infoOf(makeImage("1.0.0", "crosspoint"));
  check(!hatchSameBuild(hatchAt100, readerAt100),
        "same version, different project is NOT the same build");

  const AppImageInfo invalid;
  check(!hatchSameBuild(invalid, invalid), "two invalid descriptors are not 'the same'");
}

// --- the offer --------------------------------------------------------------

static HatchContext ctxOf(bool blobPresent, bool svOk, bool otherHasApp,
                          const char* otherVersion = "other",
                          const char* otherProject = "something",
                          uint16_t blobChip = kChipIdEsp32C3) {
  HatchContext c;
  c.blobPresent = blobPresent;
  c.blob = infoOf(makeImage("8cabf2c", "arduino-lib-builder", 0, blobChip));
  c.secureVersionAccepted = svOk;
  c.otherSlotHasApp = otherHasApp;
  if (otherHasApp) c.otherSlot = infoOf(makeImage(otherVersion, otherProject));
  return c;
}

static void testOffer() {
  printf("what the row offers\n");

  checkEq(static_cast<int>(hatchOfferFor(ctxOf(true, true, false))),
          static_cast<int>(HatchOffer::Install), "empty other slot -> Install");
  checkEq(static_cast<int>(hatchOfferFor(ctxOf(true, true, true))),
          static_cast<int>(HatchOffer::Replace), "occupied other slot -> Replace");
  checkEq(static_cast<int>(hatchOfferFor(
              ctxOf(true, true, true, "8cabf2c", "arduino-lib-builder"))),
          static_cast<int>(HatchOffer::AlreadyPresent),
          "the same build already there -> AlreadyPresent");
  checkEq(static_cast<int>(hatchOfferFor(ctxOf(false, true, false))),
          static_cast<int>(HatchOffer::Unavailable), "no blob -> Unavailable");
  checkEq(static_cast<int>(hatchOfferFor(ctxOf(true, false, false))),
          static_cast<int>(HatchOffer::RefusedRollback),
          "anti-rollback would refuse it -> RefusedRollback");
  checkEq(static_cast<int>(hatchOfferFor(
              ctxOf(true, true, false, "o", "p", 99))),
          static_cast<int>(HatchOffer::WrongChip), "wrong chip -> WrongChip");

  // Only two of the six offers may write anything.
  int actionable = 0;
  const HatchOffer all[] = {HatchOffer::Unavailable, HatchOffer::WrongChip,
                            HatchOffer::RefusedRollback, HatchOffer::AlreadyPresent,
                            HatchOffer::Install, HatchOffer::Replace};
  for (const HatchOffer o : all) {
    if (hatchOfferIsActionable(o)) actionable++;
  }
  checkEq(actionable, 2, "exactly two offers are actionable (Install, Replace)");
  check(!hatchOfferIsActionable(HatchOffer::AlreadyPresent),
        "AlreadyPresent writes nothing — 486 KB of flash wear for no change");
  check(!hatchOfferIsActionable(HatchOffer::RefusedRollback),
        "RefusedRollback writes nothing — it would claim a route that cannot boot");
}

static void testLockStateNeverChangesTheOffer() {
  printf("lock state changes the wording, never the offer\n");

  // This is the design decision from SPEC §10.7, pinned so that a later change
  // to "just hide it on unlocked devices" has to break a test and read why.
  // The fork's rule is "marca, nunca esconde" (bug #5), and lock state is a
  // weaker signal than it looks: the author's X3 has serial download ENABLED
  // and a power-only cable — unlocked, and in practice unflashable.
  for (int blob = 0; blob < 2; blob++) {
    for (int sv = 0; sv < 2; sv++) {
      for (int other = 0; other < 2; other++) {
        HatchContext unlocked = ctxOf(blob, sv, other);
        HatchContext locked = unlocked;
        unlocked.deviceLocked = false;
        locked.deviceLocked = true;
        if (hatchOfferFor(unlocked) == hatchOfferFor(locked)) continue;
        failures++;
        printf("  FAIL  lock state changed the offer (blob=%d sv=%d other=%d)\n",
               blob, sv, other);
      }
    }
  }
  check(true, "the offer is identical locked and unlocked, over every context");

  checkEq(static_cast<int>(hatchUrgencyFor(true)),
          static_cast<int>(HatchUrgency::Recommended), "locked -> Recommended");
  checkEq(static_cast<int>(hatchUrgencyFor(false)),
          static_cast<int>(HatchUrgency::Optional), "unlocked -> Optional");
}

static void testOfferTextNamesTheVictim() {
  printf("the row names what it would destroy\n");

  char buf[64];
  HatchContext c = ctxOf(true, true, true, "1.0", "crosspoint");
  hatchOfferText(c, hatchOfferFor(c), buf, sizeof(buf));
  check(std::strstr(buf, "crosspoint") != nullptr,
        "Replace names the app in the other slot");
  check(std::strstr(buf, "  ") == nullptr,
        "the field's padding is trimmed, not shown");

  c = ctxOf(true, true, false);
  hatchOfferText(c, hatchOfferFor(c), buf, sizeof(buf));
  check(std::strcmp(buf, "Install") == 0, "an empty slot just says Install");

  // Every offer produces something non-empty: a blank value column on a device
  // with no serial console is indistinguishable from a broken row.
  const HatchOffer all[] = {HatchOffer::Unavailable, HatchOffer::WrongChip,
                            HatchOffer::RefusedRollback, HatchOffer::AlreadyPresent,
                            HatchOffer::Install, HatchOffer::Replace};
  for (const HatchOffer o : all) {
    buf[0] = '\0';
    hatchOfferText(c, o, buf, sizeof(buf));
    if (buf[0] != '\0') continue;
    failures++;
    printf("  FAIL  offer %d produced an empty row value\n", static_cast<int>(o));
  }
  check(true, "every offer has text");

  // A buffer too small must truncate, not run over. ASan is watching.
  char tiny[4];
  c = ctxOf(true, true, true, "1.0", "crosspoint");
  hatchOfferText(c, HatchOffer::Replace, tiny, sizeof(tiny));
  check(std::strlen(tiny) < sizeof(tiny), "a tiny buffer is truncated safely");
}

// --- the real blob ----------------------------------------------------------

static void testTheRealBlob() {
  printf("assets/hatch.bin, the blob that actually ships\n");

  const char* path = "../assets/hatch.bin";
  FILE* f = std::fopen(path, "rb");
  if (!f) {
    failures++;
    printf("  FAIL  cannot open %s — the embedded payload is missing\n", path);
    return;
  }
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> img(static_cast<size_t>(size));
  const size_t got = std::fread(img.data(), 1, img.size(), f);
  std::fclose(f);
  checkEq(static_cast<int>(got == img.size()), 1, "the blob reads fully");

  AppImageInfo info;
  check(hatchParseImage(img.data(), img.size(), &info), "the blob parses as an app image");
  checkEq(static_cast<int>(info.chipId), kChipIdEsp32C3, "the blob is built for the ESP32-C3");
  printf("        version '%s'  project '%s'  secure_version %u  %ld bytes\n",
         info.version, info.projectName, info.secureVersion, size);

  // It has to fit the destination slot (0x640000) with room to spare, and be
  // big enough to be a real firmware rather than a stub.
  check(size > 64 * 1024, "the blob is larger than the 64 KiB minimum a firmware can be");
  check(static_cast<unsigned long>(size) < 0x640000UL, "the blob fits an OTA slot");

  // Walk the segment table and verify the ESP image XOR checksum. This is what
  // catches truncation: a short copy fails the walk or the checksum, and a
  // half-written file fails both. Layout and seed per
  // escape-hatch/tools/patch_secure_version.py.
  const uint8_t segCount = img[1];
  const bool hashAppended = img[23] != 0;
  size_t pos = kImgHeaderSize;
  uint8_t xorSum = 0xEF;  // CHECKSUM_SEED
  bool walkOk = true;
  for (uint8_t s = 0; s < segCount; s++) {
    if (pos + kImgSegHeader > img.size()) { walkOk = false; break; }
    uint32_t dataLen = 0;
    for (int b = 0; b < 4; b++) {
      dataLen |= static_cast<uint32_t>(img[pos + 4 + b]) << (8 * b);
    }
    pos += kImgSegHeader;
    if (pos + dataLen > img.size()) { walkOk = false; break; }
    for (uint32_t i = 0; i < dataLen; i++) xorSum ^= img[pos + i];
    pos += dataLen;
  }
  check(walkOk, "the segment table walks without running past the end of the file");

  if (walkOk) {
    // One checksum byte sits at the end of a 16-byte-aligned padded region.
    const size_t padded = (pos + 16) & ~static_cast<size_t>(15);
    const size_t checksumAt = padded - 1;
    check(checksumAt < img.size(), "the checksum byte is inside the file");
    if (checksumAt < img.size()) {
      checkEq(img[checksumAt], xorSum, "the image XOR checksum matches");
    }
    const size_t expected = padded + (hashAppended ? 32u : 0u);
    checkEq(static_cast<int>(img.size()), static_cast<int>(expected),
            "the declared layout accounts for every byte of the file");
  }
}

int main() {
  printf("\n=== embedded recovery firmware ===\n\n");
  testParsing();
  printf("\n");
  testSameBuild();
  printf("\n");
  testOffer();
  printf("\n");
  testLockStateNeverChangesTheOffer();
  printf("\n");
  testOfferTextNamesTheVictim();
  printf("\n");
  testTheRealBlob();
  printf("\n");

  if (failures) {
    printf("FAILURES: %d\n", failures);
    return 1;
  }
  printf("embedded recovery: every check passed\n");
  return 0;
}
