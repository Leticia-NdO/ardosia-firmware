#!/bin/sh
# Acceptance tests T3.1 and T3.2 from SPEC-release-blockers.md, as a check that
# runs instead of a checklist that gets read once.
#
# T3.1  every directory under lib/ is named in NOTICE
# T3.2  every build-time dependency — lib_deps in platformio.ini, dependencies
#       in any library.json, and the frameworks — is named in NOTICE
# T3.3  no TODO(verify) is left in NOTICE
#
# WHY THIS IS A SCRIPT: the failure it catches is silent and late. A new
# vendored library is a new copyright holder, and nothing about adding one to
# lib/ makes anybody open NOTICE. The day that happens, the released binary
# carries code whose licence is not honoured, and the only way to find out is
# for someone downstream to notice.
#
# Fonts are covered by the same idea from the other end: see the header of
# lib/EpdFont/scripts/convert-builtin-fonts.sh.

set -e
cd "$(dirname "$0")/.."

NOTICE=NOTICE
status=0

fail() {
  echo "  FAIL  $1"
  status=1
}

[ -f "$NOTICE" ] || { echo "NOTICE is missing"; exit 1; }

echo "T3.1  every lib/ directory is attributed in NOTICE"
for d in lib/*/; do
  name=$(basename "$d")
  if ! grep -qE "lib/$name([^A-Za-z0-9_-]|\$)" "$NOTICE"; then
    fail "lib/$name is not named in NOTICE"
  fi
done

echo "T3.2  every build-time dependency is attributed in NOTICE"

# lib_deps in platformio.ini: lines like "  h2zero/esp-nimble-cpp@2.3.4".
# The package name after the slash is what has to appear.
sed -n '/^lib_deps/,/^[^[:space:]]/p' platformio.ini \
  | grep -oE '[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+' \
  | while read -r dep; do
      pkg=${dep#*/}
      # -w, not a bare substring: without it "SdFat" is satisfied by "SdFatXX",
      # which is how a renamed dependency would slip through unattributed.
      grep -qw "$pkg" "$NOTICE" || echo "  FAIL  lib_deps $dep is not in NOTICE"
    done > /tmp/notice_deps.$$ || true
if [ -s /tmp/notice_deps.$$ ]; then cat /tmp/notice_deps.$$; status=1; fi
rm -f /tmp/notice_deps.$$

# dependencies{} in every bundled library.json, e.g. "greiman/SdFat".
find lib -name library.json -print \
  | while read -r man; do
      python3 - "$man" "$NOTICE" <<'PY' || echo "  FAIL  unreadable $man"
import json, re, sys
man, notice = sys.argv[1], sys.argv[2]
text = open(notice, encoding='utf-8').read()
deps = json.load(open(man, encoding='utf-8')).get('dependencies') or {}
for dep in deps:
    pkg = dep.split('/')[-1]
    # Whole word, for the same reason as the lib_deps check above.
    if not re.search(r'(?<![\w-])%s(?![\w-])' % re.escape(pkg), text):
        print("  FAIL  %s dependency %s is not in NOTICE" % (man, dep))
PY
    done > /tmp/notice_man.$$ || true
if [ -s /tmp/notice_man.$$ ]; then cat /tmp/notice_man.$$; status=1; fi
rm -f /tmp/notice_man.$$

# The two frameworks the firmware links against.
grep -q "arduino-esp32" "$NOTICE" || fail "the arduino-esp32 core is not in NOTICE"
grep -q "ESP-IDF" "$NOTICE" || fail "ESP-IDF is not in NOTICE"

echo "T3.3  no TODO(verify) is left in NOTICE"
if grep -n "TODO(verify)" "$NOTICE"; then
  fail "NOTICE still has TODO(verify) entries"
fi

# The two files that are deliberately not MIT have to say so in themselves, not
# only in NOTICE: a file travels without the repository around it.
echo "extra   the non-MIT files carry their own SPDX header"
for f in lib/EpdFont/EpdFontData.h lib/EpdFont/scripts/fontconvert.py; do
  grep -q "SPDX-License-Identifier: LGPL-3.0" "$f" \
    || fail "$f has no LGPL SPDX header"
  grep -q "$f" "$NOTICE" || fail "$f is not listed in NOTICE"
done
for t in LICENSES/LGPL-3.0.txt LICENSES/GPL-3.0.txt; do
  [ -f "$t" ] || fail "$t is missing"
done

if [ "$status" -eq 0 ]; then
  echo
  echo "NOTICE covers every library, dependency and framework."
fi
exit $status
