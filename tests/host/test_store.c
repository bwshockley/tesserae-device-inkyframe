// Host tests for the card-backed frame store (src/store.c with
// BOARD_HAS_PSRAM=0): writes are combined into aligned 4 KB runs, a read or a
// non-contiguous write flushes the pending run first, data reads back exactly,
// store_sync() reports a failed write, and the file is sized once. FatFs is a
// RAM-backed stub below that records every f_write. Run tests/host/run.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "sdcard.h"
#include "store.h"
#include "vendor/fatfs/ff.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

// ---- the log and the card (stubs) ----

void log_emit(char level, const char *fmt, ...) { (void)level; (void)fmt; }

static bool card_present = true;
bool sdcard_mount(void) { return card_present; }
bool sdcard_mounted(void) { return card_present; }
uint64_t sdcard_free_bytes(void) { return 1u << 30; }

// ---- FatFs: one file in RAM ----

static uint8_t *disk;           // STORE_BYTES, allocated on first open
static FSIZE_t disk_size;       // the file's size as FatFs would report it
static int opens, syncs, mkdirs;
static bool fail_writes;        // make f_write report an error
static struct { FSIZE_t at; UINT len; } writes[64];
static int nwrites;

FRESULT f_mkdir(const TCHAR *path) { (void)path; mkdirs++; return FR_EXIST; }

FRESULT f_open(FIL *fp, const TCHAR *path, BYTE mode)
{
    (void)path; (void)mode;
    opens++;
    if (!disk) disk = calloc(STORE_BYTES, 1);
    memset(fp, 0, sizeof *fp);
    fp->obj.objsize = disk_size;
    return FR_OK;
}

FRESULT f_close(FIL *fp) { (void)fp; return FR_OK; }
FRESULT f_sync(FIL *fp) { (void)fp; syncs++; return FR_OK; }

FRESULT f_lseek(FIL *fp, FSIZE_t ofs)
{
    if (ofs > STORE_BYTES) ofs = STORE_BYTES;   // the "card" is exactly big enough
    fp->fptr = ofs;
    if (ofs > fp->obj.objsize) { fp->obj.objsize = ofs; disk_size = ofs; }   // expanded, contents undefined
    return FR_OK;
}

FRESULT f_read(FIL *fp, void *buff, UINT btr, UINT *br)
{
    FSIZE_t left = fp->obj.objsize - fp->fptr;
    *br = btr < left ? btr : (UINT)left;
    memcpy(buff, disk + fp->fptr, *br);
    fp->fptr += *br;
    return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buff, UINT btw, UINT *bw)
{
    if (fail_writes) { *bw = 0; return FR_DISK_ERR; }
    if (nwrites < (int)(sizeof writes / sizeof writes[0])) writes[nwrites] = (typeof(writes[0])){ fp->fptr, btw };
    nwrites++;
    memcpy(disk + fp->fptr, buff, btw);
    fp->fptr += btw;
    if (fp->fptr > fp->obj.objsize) fp->obj.objsize = fp->fptr;
    *bw = btw;
    return FR_OK;
}

// ---- helpers ----

static void fill(uint8_t *p, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)((seed + i * 7) ^ (i >> 3)); }

static bool disk_matches(uint32_t at, const uint8_t *want, size_t n) { return memcmp(disk + at, want, n) == 0; }

int main(void)
{
    // ---- bring-up: the file is sized to STORE_BYTES once, then left alone ----
    CHECK(store_init(), "init with a card");
    CHECK(store_ready(), "ready after init");
    CHECK(disk_size == STORE_BYTES, "file sized to STORE_BYTES: %lu", (unsigned long)disk_size);
    CHECK(syncs == 1, "sized file synced once (%d)", syncs);
    CHECK(store_test(), "self-test");
    int syncs_after_test = syncs;
    CHECK(store_init(), "re-init");
    CHECK(syncs == syncs_after_test, "an already-sized file is not resized");

    // ---- sequential writes in TCP-segment sizes combine into aligned 4 KB runs ----
    static uint8_t frame[FRAME_BYTES], back[FRAME_BYTES];
    fill(frame, sizeof frame, 1);
    nwrites = 0;
    for (size_t off = 0; off < sizeof frame; ) {
        size_t n = sizeof frame - off < 1460 ? sizeof frame - off : 1460;
        store_write((uint32_t)off, frame + off, n);
        off += n;
    }
    int whole = FRAME_BYTES / 4096;
    CHECK(nwrites == whole, "%d whole 4 KB runs went out while streaming, got %d", whole, nwrites);
    for (int i = 0; i < nwrites && i < whole; i++)
        CHECK(writes[i].at == (FSIZE_t)i * 4096 && writes[i].len == 4096, "run %d at %lu len %u", i, (unsigned long)writes[i].at, writes[i].len);
    CHECK(!disk_matches(whole * 4096, frame + whole * 4096, FRAME_BYTES % 4096), "the tail is still pending in SRAM");

    // ---- a read flushes the tail first and sees every byte ----
    store_read(0, back, sizeof back);
    CHECK(nwrites == whole + 1, "the read flushed the tail (%d writes)", nwrites);
    CHECK(writes[whole].at == (FSIZE_t)whole * 4096 && writes[whole].len == FRAME_BYTES % 4096, "tail run at %lu len %u", (unsigned long)writes[whole].at, writes[whole].len);
    CHECK(memcmp(frame, back, sizeof frame) == 0, "frame reads back");
    CHECK(store_sync(), "sync after a clean download");

    // ---- an unaligned start: the first run stops at the 4 KB boundary ----
    uint8_t blob[6000]; fill(blob, sizeof blob, 9);
    nwrites = 0;
    store_write(RELAY_PSRAM_OFFSET + 100, blob, sizeof blob);
    CHECK(nwrites == 1 && writes[0].at == RELAY_PSRAM_OFFSET + 100 && writes[0].len == 4096 - 100,
          "first run ends on the boundary: %d writes, at +%lu len %u", nwrites, (unsigned long)(writes[0].at - RELAY_PSRAM_OFFSET), writes[0].len);
    store_sync();
    CHECK(nwrites == 2 && writes[1].at == RELAY_PSRAM_OFFSET + 4096 && writes[1].len == sizeof blob - (4096 - 100), "the rest followed aligned");
    CHECK(disk_matches(RELAY_PSRAM_OFFSET + 100, blob, sizeof blob), "unaligned blob landed");

    // ---- a non-contiguous write flushes what was pending ----
    uint8_t a[10] = "aaaaaaaaa", b[10] = "bbbbbbbbb";
    nwrites = 0;
    store_write(CACHE_PSRAM_OFFSET, a, 10);
    CHECK(nwrites == 0, "a short write waits");
    store_write(CACHE_PSRAM_OFFSET + 1000, b, 10);
    CHECK(nwrites == 1 && writes[0].at == CACHE_PSRAM_OFFSET && writes[0].len == 10, "the jump flushed the first write");
    store_sync();
    CHECK(nwrites == 2 && disk_matches(CACHE_PSRAM_OFFSET, a, 10) && disk_matches(CACHE_PSRAM_OFFSET + 1000, b, 10), "both landed");

    // ---- a write that spans two runs exactly at a boundary ----
    uint8_t span[8192]; fill(span, sizeof span, 3);
    nwrites = 0;
    store_write(OTA_LOG_CARRY_PSRAM_OFFSET, span, sizeof span);
    CHECK(nwrites == 2, "two aligned runs for 8 KB at a boundary (%d)", nwrites);
    store_read(OTA_LOG_CARRY_PSRAM_OFFSET, back, sizeof span);
    CHECK(memcmp(span, back, sizeof span) == 0, "8 KB reads back");

    // ---- a failed card write is reported by the next sync, then cleared ----
    fail_writes = true;
    store_write(0, frame, 5000);          // the first run goes out (and fails) at the boundary
    CHECK(!store_sync(), "sync reports the failed write");
    fail_writes = false;
    CHECK(store_sync(), "a clean sync follows");

    // ---- no card ----
    card_present = false;
    CHECK(!store_init(), "init without a card");
    CHECK(!store_ready(), "not ready without a card");
    CHECK(!store_sync(), "sync fails without a store");
    memset(back, 0xEE, 16);
    store_read(0, back, 16);
    CHECK(back[0] == 0 && back[15] == 0, "reads are zero without a store");
    CHECK(!store_test(), "self-test fails without a store");

    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    puts("store: all tests passed");
    return 0;
}
