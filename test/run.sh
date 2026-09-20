#!/bin/sh
# Host-side tests. No PlatformIO, no device.
#
# Compiles the REAL firmware sources — not copies — so the tests cannot drift
# from the code they check. The firmware build ignores test/
# (src/CMakeLists.txt globs src/ only). Do not run `pio test`: no Unity here.
#
# ASan/UBSan are used when their runtime is installed (dnf install libasan
# libubsan) and quietly skipped when it is not, so this always runs.
set -e
cd "$(dirname "$0")"

CXX=${CXX:-g++}
BASE="-std=gnu++17 -Wall -Wextra -O1 -g"
SAN="-fsanitize=address,undefined"

if $CXX $BASE $SAN -x c++ /dev/null -o /dev/null 2>/dev/null; then
  FLAGS="$BASE $SAN"
  echo "(built with AddressSanitizer + UBSan)"
else
  FLAGS="$BASE"
  echo "(sanitizer runtime unavailable - building without it; 'dnf install libasan libubsan' to enable)"
fi

status=0

$CXX $FLAGS -o /tmp/microslate_test_ptbr \
    test_ptbr.cpp ../src/utf8_util.cpp ../src/deadkeys.cpp ../src/keymap.cpp ../src/text_editor.cpp
/tmp/microslate_test_ptbr || status=1

echo
$CXX $FLAGS -o /tmp/microslate_test_sleep \
    test_sleep.cpp ../src/sleep_layout.cpp
/tmp/microslate_test_sleep || status=1

exit $status
