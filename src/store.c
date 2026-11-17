#include "store.h"

#include <string.h>

#include "config.h"
#include "log.h"

#if BOARD_HAS_PSRAM

// ---- the 7.3": SPI PSRAM ----

#include "psram.h"

static bool s_ready;

bool store_init(void) { s_ready = psram_init(); return s_ready; }
bool store_ready(void) { return s_ready; }
void store_write(uint32_t addr, const uint8_t *data, size_t len) { psram_write(addr, data, len); }
void store_read(uint32_t addr, uint8_t *data, size_t len) { psram_read(addr, data, len); }
bool store_sync(void) { return s_ready; }
bool store_test(void) { return psram_test(); }

#else

// ---- the 4.0" and 5.7": a file on the microSD card ----

#include "sdcard.h"
#include "vendor/fatfs/ff.h"

#define STORE_DIR   "/tesserae"
#define STORE_PATH  STORE_DIR "/store.bin"
#define RUN_BYTES   4096u

static FIL s_file;
static bool s_ready;
static bool s_failed;           // a write or sync failed since the last store_sync()
static uint8_t s_run[RUN_BYTES];
static uint32_t s_run_addr;     // where the pending run starts in the store
static size_t s_run_len;

static void fail(const char *what, FRESULT r)
{
    if (!s_failed) LOGE("store: %s failed (FatFs %d)", what, r);
    s_failed = true;
}

static void flush(void)
{
    if (!s_run_len) return;
    size_t len = s_run_len;
    s_run_len = 0;
    FRESULT r = f_lseek(&s_file, s_run_addr);
    if (r != FR_OK) { fail("seek", r); return; }
    UINT put = 0;
    r = f_write(&s_file, s_run, (UINT)len, &put);
    if (r != FR_OK || put != len) fail("write", r);
}

bool store_init(void)
{
    s_ready = false;
    s_failed = false;
    s_run_len = 0;
    if (!sdcard_mount()) {
        LOGE("store: no microSD card; this frame keeps its frame store on the card");
        return false;
    }
    f_mkdir(STORE_DIR);
    FRESULT r = f_open(&s_file, STORE_PATH, FA_READ | FA_WRITE | FA_OPEN_ALWAYS);
    if (r != FR_OK) { LOGE("store: cannot open " STORE_PATH " (FatFs %d)", r); return false; }
    if (f_size(&s_file) < STORE_BYTES) {
        // Size the file once. Seeking past the end allocates its clusters
        // without writing them; on a full card the pointer stops short.
        r = f_lseek(&s_file, STORE_BYTES);
        if (r == FR_OK && f_tell(&s_file) != STORE_BYTES) r = FR_DENIED;
        if (r == FR_OK) r = f_sync(&s_file);
        if (r != FR_OK) {
            LOGE("store: cannot size " STORE_PATH " to %lu KB (FatFs %d); is the card full?", (unsigned long)(STORE_BYTES / 1024u), r);
            f_close(&s_file);
            return false;
        }
    }
    s_ready = true;
    LOGI("store: " STORE_PATH ", %lu KB", (unsigned long)(STORE_BYTES / 1024u));
    return true;
}

bool store_ready(void) { return s_ready; }

void store_write(uint32_t addr, const uint8_t *data, size_t len)
{
    if (!s_ready) { s_failed = true; return; }
    while (len) {
        if (s_run_len && addr != s_run_addr + s_run_len) flush();
        if (!s_run_len) s_run_addr = addr;
        // Fill the run up to the next 4 KB boundary of the store, so every
        // flush but the first of a sequence is a whole, aligned run.
        size_t room = RUN_BYTES - ((s_run_addr + s_run_len) & (RUN_BYTES - 1));
        size_t n = len < room ? len : room;
        memcpy(s_run + s_run_len, data, n);
        s_run_len += n; addr += n; data += n; len -= n;
        if (((s_run_addr + s_run_len) & (RUN_BYTES - 1)) == 0) flush();
    }
}

void store_read(uint32_t addr, uint8_t *data, size_t len)
{
    if (!s_ready) { memset(data, 0, len); return; }
    flush();
    UINT got = 0;
    FRESULT r = f_lseek(&s_file, addr);
    if (r == FR_OK) r = f_read(&s_file, data, (UINT)len, &got);
    if (r != FR_OK || got != len) {
        fail("read", r);
        memset(data + got, 0, len - got);
    }
}

bool store_sync(void)
{
    if (!s_ready) return false;
    flush();
    FRESULT r = f_sync(&s_file);
    if (r != FR_OK) fail("sync", r);
    bool ok = !s_failed;
    s_failed = false;
    return ok;
}

bool store_test(void)
{
    if (!s_ready) return false;
    uint8_t pat[256], back[256];
    for (int i = 0; i < 256; i++) pat[i] = (uint8_t)(i ^ 0xA5);
    // Past the frame, inside its region's slack, and across a 4 KB boundary
    // so both the partial first run and the aligned run that follows are tried.
    uint32_t addr = (FRAME_BYTES / RUN_BYTES + 1u) * RUN_BYTES - 100u;
    store_write(addr, pat, 100);
    store_write(addr + 100, pat + 100, sizeof pat - 100);
    store_read(addr, back, sizeof back);
    return store_sync() && memcmp(pat, back, sizeof pat) == 0;
}

#endif
