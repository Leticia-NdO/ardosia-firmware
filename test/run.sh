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

$CXX $FLAGS -o /tmp/ardosia_test_ptbr \
    test_ptbr.cpp ../src/utf8_util.cpp ../src/deadkeys.cpp ../src/keymap.cpp ../src/text_editor.cpp
/tmp/ardosia_test_ptbr || status=1

echo
$CXX $FLAGS -DRELEASE_BUILD -I host_stubs -I ../src \
    -o /tmp/ardosia_test_files \
    test_files.cpp host_stubs/SDCardManager.cpp \
    ../src/utf8_util.cpp ../src/file_manager.cpp ../src/text_editor.cpp
/tmp/ardosia_test_files || status=1

echo
$CXX $FLAGS -o /tmp/ardosia_test_sleep \
    test_sleep.cpp ../src/sleep_layout.cpp
/tmp/ardosia_test_sleep || status=1

echo
$CXX $FLAGS -o /tmp/ardosia_test_menu \
    test_menu.cpp ../src/keymap.cpp
/tmp/ardosia_test_menu || status=1

echo
$CXX $FLAGS -o /tmp/ardosia_test_ui_layout \
    test_ui_layout.cpp ../src/ui_layout.cpp
/tmp/ardosia_test_ui_layout || status=1

echo
$CXX $FLAGS -o /tmp/ardosia_test_dpad \
    test_dpad.cpp ../src/dpad.cpp
/tmp/ardosia_test_dpad || status=1

echo
$CXX $FLAGS -o /tmp/ardosia_test_confirm \
    test_confirm.cpp ../src/confirm.cpp
/tmp/ardosia_test_confirm || status=1

echo
$CXX $FLAGS -o /tmp/ardosia_test_quickmenu \
    test_quickmenu.cpp ../src/quickmenu.cpp
/tmp/ardosia_test_quickmenu || status=1

echo
$CXX $FLAGS -o /tmp/ardosia_test_hotspot_creds \
    test_hotspot_creds.cpp ../src/hotspot_creds.cpp
/tmp/ardosia_test_hotspot_creds || status=1

echo
$CXX $FLAGS -DRELEASE_BUILD -DARDOSIA_HOST_INPUT -I host_stubs -I ../lib/InputManager/include \
    -o /tmp/ardosia_test_buttons \
    test_buttons.cpp ../lib/InputManager/src/InputManager.cpp
/tmp/ardosia_test_buttons || status=1

echo
$CXX $FLAGS -I ../src -o /tmp/ardosia_test_escape \
    test_escape.cpp ../src/escape.cpp
/tmp/ardosia_test_escape || status=1

echo
$CXX $FLAGS -I ../src -o /tmp/ardosia_test_hatch \
    test_hatch.cpp ../src/hatch.cpp
/tmp/ardosia_test_hatch || status=1

echo
$CXX $FLAGS -I ../src -o /tmp/ardosia_test_ota_names \
    test_ota_names.cpp ../src/ota_names.cpp
/tmp/ardosia_test_ota_names || status=1

echo
$CXX $FLAGS -I ../src -o /tmp/ardosia_test_note_json \
    test_note_json.cpp ../src/note_json.cpp
/tmp/ardosia_test_note_json || status=1

echo
$CXX $FLAGS -I ../src -o /tmp/ardosia_test_note_html \
    test_note_html.cpp ../src/note_html.cpp
/tmp/ardosia_test_note_html || status=1

echo
$CXX $FLAGS -I ../src -o /tmp/ardosia_test_zip \
    test_zip.cpp ../src/zip_writer.cpp
/tmp/ardosia_test_zip || status=1

# The archive the test above just wrote, opened by an INDEPENDENT reader.
# Asserting my own bytes against my own expectations proves only that I am
# self-consistent; Python's zipfile verifying the CRCs and handing back the
# contents proves the output is a ZIP. Skipped in silence without python3, the
# same way the sanitizers are.
if command -v python3 >/dev/null 2>&1; then
  echo
  python3 ./check-zip.py || status=1
  # Likewise for the page the test above wrote: an unclosed tag in that <style>
  # block is a blank page on someone's phone and nothing at all on this device,
  # which has no way to serve a page to a validator.
  echo
  python3 ./check-page.py || status=1
else
  echo
  echo "(python3 unavailable - skipping the independent ZIP and page read-backs)"
fi

# Not a C++ test: the attribution of every vendored library and build-time
# dependency. Here because it is the same kind of thing — a property of the tree
# that nobody re-checks by hand, and whose breakage is invisible until someone
# downstream finds it.
echo
echo "=== NOTICE coverage ==="
./check-notice.sh || status=1

exit $status
