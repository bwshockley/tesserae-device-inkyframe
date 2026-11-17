// 8 MB SPI PSRAM (APS6404-class) on the display SPI bus, CS GP3: the 7.3"
// Frame's frame store, behind store.h (the 4.0" and 5.7" have none and use
// the card instead). Sequences follow Pimoroni's psram_display driver.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool psram_init(void);
void psram_write(uint32_t addr, const uint8_t *data, size_t len);
void psram_fill(uint32_t addr, uint8_t byte, size_t len);
void psram_read(uint32_t addr, uint8_t *data, size_t len);
// Write/read self-test over a small region; returns true on success.
bool psram_test(void);
