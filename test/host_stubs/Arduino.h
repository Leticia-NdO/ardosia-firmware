#pragma once

// Host-test stand-in. file_manager.cpp includes <Arduino.h> for the firmware
// DBG macros; with -DRELEASE_BUILD those expand to nothing, so this only
// needs the C library bits snprintf would otherwise get from Arduino.
#include <cstdio>
#include <cstring>
#include <cstdint>
