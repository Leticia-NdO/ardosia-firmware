# assets/

Binary payloads embedded into the firmware image by `src/CMakeLists.txt`
(`EMBED_FILES`). **Not** a source directory — `src/CMakeLists.txt` globs
`src/*.*`, so anything with a dot placed in `src/` would be handed to the
compiler as a translation unit. That is why these live here.

## hatch.bin

The Escape Hatch recovery firmware: a minimal SD-card flasher with a file
browser, a "Boot Other Slot" item and an eFuse readout, and nothing else. Built
from the `escape-hatch` project (FreeInk SDK + Arduino core, MIT).

Ardosia can write this into the unused OTA slot from
`Settings > System > Install Recovery`, so a USB-locked device ends up with a
real recovery firmware it can always reach with Back + Up. See
`src/hatch.h` for the decision logic and `../SPEC-firmware-distribution.md` §10
for why it is embedded rather than downloaded.

Verified by `test/test_hatch.cpp`, which parses this exact file: ESP image
magic, chip id matching the ESP32-C3, a segment-table walk, the XOR checksum
and the declared-vs-actual file size. That test is what catches a truncated or
wrong-chip blob at build time instead of on the glass.

### Replacing it

```sh
cp ../../escape-hatch/.pio/build/default/firmware.bin assets/hatch.bin
./test/run.sh          # test_hatch re-validates the new blob
```

Do **not** substitute `escape-hatch/patched bins/eh_v999.bin` here. That variant
exists to clear the *stock Xteink flasher's* app-version check during the
first-ever install on a locked device; writing it from Ardosia goes straight to
the partition and never meets that check, so the honest build — whose `version`
is its real git hash — is the right one to carry.
