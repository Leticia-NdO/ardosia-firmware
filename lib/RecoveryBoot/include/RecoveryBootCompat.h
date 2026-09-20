#pragma once

// -----------------------------------------------------------------------------
// RecoveryBoot compatibility shim — Ardosia fork
// -----------------------------------------------------------------------------
// RecoveryBoot comes from the FreeInk SDK, which builds against the pioarduino
// platform (Arduino core 3.x / ESP-IDF 5.x). Ardosia builds against
// espressif32 @ 6.12.0 (Arduino core 2.x / ESP-IDF 4.4), where two APIs the SDK
// relies on live somewhere else. This header papers over both so the SDK
// sources need only swap one #include.
//
//  1. SPI_FLASH_SEC_SIZE moved. ESP-IDF 5.0 split the mmap API into
//     spi_flash_mmap.h; under 4.4 the constant is in esp_spi_flash.h.
//
//  2. mbedTLS changed its SHA-256 API. mbedTLS 2.x (IDF 4.4) exposes the
//     error-returning calls with a _ret suffix; mbedTLS 3.x (IDF 5.x) dropped
//     the suffix. The non-suffixed names exist in 2.x only as deprecated
//     wrappers, and ESP-IDF can be configured to remove them
//     (MBEDTLS_DEPRECATED_REMOVED), so map to the _ret forms explicitly rather
//     than relying on the wrappers being present.
//
// Both branches are compile-time. Remove this file if the fork is ever rebased
// onto a MicroSlate that has moved to ESP-IDF 5.x.
// -----------------------------------------------------------------------------

#include <esp_idf_version.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <spi_flash_mmap.h>
#else
#include <esp_spi_flash.h>
#endif

#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

// Must come after mbedtls/sha256.h: these macros rewrite call sites only, and
// the real declarations have to be visible first.
#if defined(MBEDTLS_VERSION_MAJOR) && MBEDTLS_VERSION_MAJOR < 3
#define mbedtls_sha256_starts mbedtls_sha256_starts_ret
#define mbedtls_sha256_update mbedtls_sha256_update_ret
#define mbedtls_sha256_finish mbedtls_sha256_finish_ret
#endif
