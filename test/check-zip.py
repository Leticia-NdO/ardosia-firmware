#!/usr/bin/env python3
"""Open the archive test_zip.cpp wrote, with an implementation that is not ours.

test_zip.cpp checks the bytes against what it expects them to be, which only
proves self-consistency. This opens the same file with Python's zipfile, lets it
verify every CRC, and compares the contents against the manifest the C++ side
emitted. If the firmware's writer disagrees with the rest of the world about
what a ZIP is, this is what says so.
"""
import binascii
import sys
import zipfile

ZIP = "/tmp/ardosia_test.zip"
MANIFEST = "/tmp/ardosia_test.zip.manifest"

print("=== ZIP read-back (independent reader) ===")
print()

failures = 0


def check(ok, what):
    global failures
    if ok:
        print(f"  ok    {what}")
    else:
        failures += 1
        print(f"  FAIL  {what}")


try:
    want = []
    with open(MANIFEST, "rb") as f:
        for line in f.read().decode("utf-8").splitlines():
            name, size, crc = line.split("\t")
            want.append((name, int(size), int(crc, 16)))
except OSError as e:
    print(f"  FAIL  cannot read the manifest: {e}")
    sys.exit(1)

try:
    with zipfile.ZipFile(ZIP) as z:
        check(True, "Python's zipfile opens it at all")

        # testzip() recomputes every entry's CRC and returns the first bad name.
        bad = z.testzip()
        check(bad is None, f"every CRC verifies (testzip said {bad!r})")

        names = z.namelist()
        check(names == [w[0] for w in want],
              "the names and their order match what was written")

        for name, size, crc in want:
            info = z.getinfo(name)
            check(info.compress_type == zipfile.ZIP_STORED,
                  f"{name}: stored, not deflated")
            check(info.file_size == size, f"{name}: size {info.file_size} == {size}")
            check(info.CRC == crc, f"{name}: CRC {info.CRC:08X} == {crc:08X}")
            body = z.read(name)
            check(len(body) == size, f"{name}: read back {len(body)} bytes")
            check(binascii.crc32(body) & 0xFFFFFFFF == crc,
                  f"{name}: the bytes that came back hash to the same CRC")

        # The non-ASCII names are the ones a wrong flag mangles.
        check("diário.txt" in names, "an accented name survives the round trip")
        check("última.txt" in names, "and so does another")
except zipfile.BadZipFile as e:
    print(f"  FAIL  not a valid ZIP: {e}")
    failures += 1
except OSError as e:
    print(f"  FAIL  cannot open {ZIP}: {e}")
    failures += 1

print()
if failures:
    print(f"FAILURES: {failures}")
    sys.exit(1)
print("ZIP read-back: every check passed")
