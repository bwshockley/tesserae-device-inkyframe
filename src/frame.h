// Frame download into the frame store (store.h) and hand-off to the panel.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Download `url` (must be exactly FRAME_BYTES long) into the frame store.
// Returns the HTTP status (200 = complete frame in the store), or a negative error.
int frame_download(const char *url);
size_t frame_received(void);
// A frame was put into the store some other way (the relay opens a sealed one).
void frame_set_complete(bool complete);
// Paint the frame in the store. Call only after a complete download.
bool frame_paint(void);
