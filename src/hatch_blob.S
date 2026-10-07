/*
 * The embedded Escape Hatch recovery firmware (assets/hatch.bin), placed in
 * flash by hand with .incbin.
 *
 * WHY NOT the two mechanisms that exist for this:
 *
 *   board_build.embed_files — with `espidf` in PIOFRAMEWORK, the platform
 *   script (_embed_files.py:172-179) generates $BUILD_DIR/hatch.bin.S and makes
 *   the ELF *depend* on it, but never adds it to the link. It expects the
 *   ESP-IDF side to compile it.
 *
 *   idf_component_register(EMBED_FILES ...) — the ESP-IDF side, in turn, expects
 *   the file to already exist when CMake configures, and PlatformIO generates it
 *   later. That fails with "Source `.pio/build/xteink_x4/hatch.bin.S' not found".
 *
 * Each half waits for the other. .incbin sidesteps both: the assembler pulls the
 * bytes in at assemble time, so there is no generated source to order, nothing
 * to gitignore, and no 3 MB of hex in the tree — the payload stays a binary in
 * assets/ where test_hatch.cpp can validate it.
 *
 * Everything under src/ with a dot in its name is globbed into the IDF
 * component by src/CMakeLists.txt, which is how
 * this file gets assembled; -Iassets in platformio.ini is how .incbin finds the
 * blob. The symbol names match what the two mechanisms above would have
 * produced, so src/hatch_flash.cpp does not care which route was taken.
 */

    .section .rodata.embedded
    .balign 4

    .global _binary_hatch_bin_start
_binary_hatch_bin_start:
    .incbin "hatch.bin"

    .global _binary_hatch_bin_end
_binary_hatch_bin_end:
    .balign 4
