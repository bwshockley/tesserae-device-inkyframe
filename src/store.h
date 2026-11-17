// The frame store: the byte-addressed scratch memory the frame, staged
// downloads and the OTA image live in, since none of them fit the Pico's
// SRAM. On the 7.3" it is the 8 MB SPI PSRAM (psram.c); the 4.0" and 5.7"
// have no PSRAM, so it is a file on the microSD card (BOARD_HAS_PSRAM in
// config.h). The layout of the regions is at the end of config.h.
//
// Writes to the card store are combined in SRAM and go out in whole 4 KB
// runs, so a download's TCP segments cost one card write per 4 KB rather
// than a read-modify-write each. A write that is not contiguous with the
// pending run, any read, and store_sync() flush it first.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Bring the store up: the PSRAM, or the card file (mounting the card and
// sizing the file once). False if there is none; the other calls are then
// inert and store_sync() reports failure.
bool store_init(void);
bool store_ready(void);
void store_write(uint32_t addr, const uint8_t *data, size_t len);
void store_read(uint32_t addr, uint8_t *data, size_t len);
// Push pending writes out and report whether every write since the last
// sync landed. Call it before trusting a download in the store (and before
// a reboot that must find the OTA log carry).
bool store_sync(void);
// Write and read back a pattern outside the frame region.
bool store_test(void);
