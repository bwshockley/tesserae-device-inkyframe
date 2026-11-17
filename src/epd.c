#include "epd.h"

#include <string.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "board.h"
#include "config.h"
#include "log.h"

// The 4.0" and 5.7" panels are driven by a UC8159; the 7.3" ones by an
// AC073TC1A (ACeP) or E673 (Spectra 6). The UC8159 takes its commands
// without a data byte and, per Pimoroni, a slower clock.
#if PANEL_VARIANT == PANEL_4 || PANEL_VARIANT == PANEL_57
#define EPD_UC8159 1
#else
#define EPD_UC8159 0
#endif

#define EPD_SPI   spi0
#if EPD_UC8159
#define EPD_BAUD  3000000u
#else
#define EPD_BAUD  20000000u
#endif

enum {
    PSR = 0x00, PWR = 0x01, POF = 0x02, PFS = 0x03, PON = 0x04,
    BTST1 = 0x05, BTST2 = 0x06, DSLP = 0x07, BTST3 = 0x08,   // BTST2 is the UC8159's only BTST
    DTM1 = 0x10, DSP = 0x11, DRF = 0x12, IPC = 0x13,
    PLL = 0x30, TSC = 0x40, TSE = 0x41, TSW = 0x42, TSR = 0x43,
    CDI = 0x50, LPD = 0x51, TCON = 0x60, TRES = 0x61, DAM = 0x65,
    REV = 0x70, FLG = 0x71, AMV = 0x80, VV = 0x81, VDCS = 0x82, T_VDCS = 0x84,
    AGID = 0x86, CMDH = 0xAA, CCSET = 0xE0, PWS = 0xE3, TSSET = 0xE6,
};

static uint32_t s_cpsr, s_cr0;

static inline void cs(bool low) { gpio_put(PIN_EINK_CS, !low); }
static inline void dc_data(bool data) { gpio_put(PIN_EINK_DC, data); }

static void claim(uint32_t *cpsr, uint32_t *cr0)
{
    *cpsr = spi_get_hw(EPD_SPI)->cpsr;
    *cr0 = spi_get_hw(EPD_SPI)->cr0;
    if (*cpsr != s_cpsr || *cr0 != s_cr0) {
        spi_get_hw(EPD_SPI)->cpsr = s_cpsr;
        spi_get_hw(EPD_SPI)->cr0 = s_cr0;
    }
}

static void release(uint32_t cpsr, uint32_t cr0)
{
    if (cpsr != s_cpsr || cr0 != s_cr0) {
        spi_get_hw(EPD_SPI)->cpsr = cpsr;
        spi_get_hw(EPD_SPI)->cr0 = cr0;
    }
}

static bool busy_wait(uint32_t timeout_ms)
{
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (board_epd_busy()) {
        if (time_reached(deadline)) return false;
        sleep_ms(2);
    }
    return true;
}

static void command(uint8_t reg, const uint8_t *data, size_t len)
{
    cs(true);
    dc_data(false);
    spi_write_blocking(EPD_SPI, &reg, 1);
    if (len) {
        dc_data(true);
        spi_write_blocking(EPD_SPI, data, len);
    }
    cs(false);
}

#define CMD(reg, ...) do { static const uint8_t d_[] = { __VA_ARGS__ }; command(reg, d_, sizeof d_); } while (0)
#define CMD0(reg)     command(reg, NULL, 0)

static void reset(void)
{
    gpio_put(PIN_EINK_RESET, 0); sleep_ms(10);
    gpio_put(PIN_EINK_RESET, 1); sleep_ms(10);
    busy_wait(EPD_REFRESH_TIMEOUT_MS);
}

void epd_init(void)
{
    uint32_t prev_cpsr = spi_get_hw(EPD_SPI)->cpsr, prev_cr0 = spi_get_hw(EPD_SPI)->cr0;
    spi_init(EPD_SPI, EPD_BAUD);
    s_cpsr = spi_get_hw(EPD_SPI)->cpsr;
    s_cr0 = spi_get_hw(EPD_SPI)->cr0;

    gpio_init(PIN_EINK_DC);    gpio_set_dir(PIN_EINK_DC, GPIO_OUT);
    gpio_init(PIN_EINK_CS);    gpio_set_dir(PIN_EINK_CS, GPIO_OUT);    gpio_put(PIN_EINK_CS, 1);
    gpio_init(PIN_EINK_RESET); gpio_set_dir(PIN_EINK_RESET, GPIO_OUT); gpio_put(PIN_EINK_RESET, 1);
    gpio_set_function(PIN_SPI_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SPI_MOSI, GPIO_FUNC_SPI);
    if (prev_cpsr != 0) release(prev_cpsr, prev_cr0);
}

static void setup(void)
{
    reset();
#if PANEL_VARIANT == PANEL_73_SPECTRA
    // Spectra 6 E673 (pimoroni/inky inky_e673.py)
    CMD(CMDH, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);
    CMD(PWR, 0x3F);
    // inky_e673.py's value. The Frame's 180-degree turn is done while
    // streaming (EPD_ROTATE_180 in epd_paint()), which does not depend on how
    // the controller treats PSR's scan bits.
    CMD(PSR, 0x5F, 0x69);
    CMD(BTST1, 0x40, 0x1F, 0x1F, 0x2C);
    CMD(BTST3, 0x6F, 0x1F, 0x1F, 0x22);
    CMD(BTST2, 0x6F, 0x1F, 0x17, 0x17);
    CMD(PFS, 0x00, 0x54, 0x00, 0x44);
    CMD(TCON, 0x02, 0x00);
    CMD(PLL, 0x08);
    CMD(CDI, 0x3F);
    CMD(TRES, 0x03, 0x20, 0x01, 0xE0);
    CMD(PWS, 0x2F);
    CMD(VDCS, 0x01);
#elif PANEL_VARIANT == PANEL_73_ACEP
    // 7-colour AC073TC1A (pimoroni-pico drivers/inky73, ROTATE_0)
    CMD(CMDH, 0x49, 0x55, 0x20, 0x08, 0x09, 0x18);
    CMD(PWR, 0x3F, 0x00, 0x32, 0x2A, 0x0E, 0x2A);
    CMD(PSR, 0x53, 0x69);
    CMD(PFS, 0x00, 0x54, 0x00, 0x44);
    CMD(BTST1, 0x40, 0x1F, 0x1F, 0x2C);
    CMD(BTST2, 0x6F, 0x1F, 0x16, 0x25);
    CMD(BTST3, 0x6F, 0x1F, 0x1F, 0x22);
    CMD(IPC, 0x00, 0x04);
    CMD(PLL, 0x02);
    CMD(TSE, 0x00);
    CMD(CDI, 0x3F);
    CMD(TCON, 0x02, 0x00);
    CMD(TRES, 0x03, 0x20, 0x01, 0xE0);
    CMD(VDCS, 0x1E);
    CMD(T_VDCS, 0x00);
    CMD(AGID, 0x00);
    CMD(PWS, 0x2F);
    CMD(CCSET, 0x00);
    CMD(TSSET, 0x00);
#else
    // 7-colour UC8159 (pimoroni-pico drivers/uc8159, ROTATE_0: the setting
    // Pimoroni's own Inky Frame firmware uses for both sizes). PSR's top two
    // bits pick the resolution: 0b11 for 600x448, 0b10 for 640x400; the low
    // nibble (0x3) is the scan direction, so a frame that comes out upside
    // down wants 0xEF / 0xAF here rather than a software turn.
#if PANEL_VARIANT == PANEL_57
    CMD(PSR, 0xE3, 0x08);
#else
    CMD(PSR, 0xA3, 0x08);
#endif
    CMD(PWR, 0x37, 0x00, 0x23, 0x23);
    CMD(PFS, 0x00);
    CMD(BTST2, 0xC7, 0xC7, 0x1D);
    CMD(PLL, 0x3C);
    CMD(TSC, 0x00);
    CMD(CDI, 0x37);
    CMD(TCON, 0x22);
    CMD(TRES, PANEL_W >> 8, PANEL_W & 0xFF, PANEL_H >> 8, PANEL_H & 0xFF);
    CMD(PWS, 0xAA);
    sleep_ms(100);
    CMD(CDI, 0x37);
#endif
}

// The Spectra 6 Frame mounts the E673 so that its scan starts at the
// dashboard's bottom-right: send the rows last to first, each reversed.
#if PANEL_VARIANT == PANEL_73_SPECTRA
#define EPD_ROTATE_180 1
#else
#define EPD_ROTATE_180 0
#endif

// Reverse a row of 4 bpp pixels in place (high nibble = left pixel).
static void reverse_row(uint8_t *line)
{
    for (int i = 0, j = ROW_BYTES - 1; i <= j; i++, j--) {
        uint8_t a = line[i], b = line[j];
        line[i] = (uint8_t)((b << 4) | (b >> 4));
        line[j] = (uint8_t)((a << 4) | (a >> 4));
    }
}

bool epd_paint(epd_row_fn row, void *arg)
{
    uint32_t c, r;
    claim(&c, &r);
    uint32_t t0 = to_ms_since_boot(get_absolute_time());

    busy_wait(EPD_REFRESH_TIMEOUT_MS);
    setup();

    // Data transfer: one DTM1 command, then every row as data.
    //
    // The panel's CS must be RELEASED while each row is fetched: the frame
    // store is PSRAM or the SD card on the same SPI bus, so its command and dummy bytes would
    // otherwise be clocked into the panel and shift the image. Pimoroni's
    // driver streams the same way (CS asserted only around each chunk), and
    // the controller keeps its write pointer across the gaps.
    static uint8_t line[ROW_BYTES];
    cs(true);
    dc_data(false);
    uint8_t reg = DTM1;
    spi_write_blocking(EPD_SPI, &reg, 1);
    dc_data(true);
#if PANEL_VARIANT == PANEL_4
    // The UC8159's RAM is 448 rows deep and ROTATE_0 scans it from the far
    // end, so the first 48 rows sent fall off the 400-row panel. Pimoroni's
    // driver pads the same way; the content does not matter.
    memset(line, (PAL_WHITE << 4) | PAL_WHITE, sizeof line);
    for (int y = 0; y < 448 - PANEL_H; y++) spi_write_blocking(EPD_SPI, line, ROW_BYTES);
#endif
    cs(false);
    for (int y = 0; y < PANEL_H; y++) {
        if (EPD_ROTATE_180) {
            row(PANEL_H - 1 - y, line, arg);   // may drive the shared bus (frame store)
            reverse_row(line);
        } else {
            row(y, line, arg);                 // may drive the shared bus (frame store)
        }
        uint32_t pc, pr;
        claim(&pc, &pr);            // restore the panel's clock and format
        cs(true);
        spi_write_blocking(EPD_SPI, line, ROW_BYTES);
        cs(false);
    }
    dc_data(false);

    // Power on, refresh, power off. The UC8159 takes these bare; the 7.3"
    // controllers want a data byte, the Spectra a booster tweak first, and
    // both a final PSR.
    busy_wait(EPD_REFRESH_TIMEOUT_MS);
#if EPD_UC8159
    CMD0(PON);
    busy_wait(EPD_REFRESH_TIMEOUT_MS);
    CMD0(DRF);
    sleep_ms(50);
    bool ok = busy_wait(EPD_REFRESH_TIMEOUT_MS);
    CMD0(POF);
    busy_wait(5000);
#else
    CMD(PON, 0x00);
    busy_wait(EPD_REFRESH_TIMEOUT_MS);
#if PANEL_VARIANT == PANEL_73_SPECTRA
    CMD(BTST2, 0x6F, 0x1F, 0x17, 0x49);
#endif
    CMD(DRF, 0x00);
    sleep_ms(50);
    bool ok = busy_wait(EPD_REFRESH_TIMEOUT_MS);
    CMD(POF, 0x00);
    busy_wait(5000);
    CMD(PSR, 0x4F, 0x6E);
#endif

    release(c, r);
    LOGI("epd: refresh %s in %lu ms", ok ? "done" : "TIMED OUT",
         (unsigned long)(to_ms_since_boot(get_absolute_time()) - t0));
    return ok;
}

static void test_row(int y, uint8_t *out, void *arg)
{
    (void)arg;
    // Eight equal bars of index 0..7 (100 px each on the 7.3"); a black band
    // across the top so the top edge is obvious, white band across the bottom.
    for (int x = 0; x < PANEL_W; x += 2) {
        uint8_t a, b;
        if (y < 24) a = b = PAL_BLACK;
        else if (y >= PANEL_H - 24) a = b = PAL_WHITE;
        else { a = (uint8_t)(x * 8 / PANEL_W); b = (uint8_t)((x + 1) * 8 / PANEL_W); }
        out[x / 2] = (uint8_t)((a << 4) | (b & 0x0F));
    }
}

bool epd_paint_test_pattern(void)
{
    return epd_paint(test_row, NULL);
}
