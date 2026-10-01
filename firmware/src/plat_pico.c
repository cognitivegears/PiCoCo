#include "plat.h"
#include "pico/stdlib.h"
#include "pico/aon_timer.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include PICOCO_BOARD_H
#include "fs_flash.h"
#include "usb_descriptors.h"
#include "ff.h"
#include "crash.h"
#include <stdio.h>

uint32_t plat_now_us(void) { return time_us_32(); }
uint32_t plat_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

/* watchdog_enable() unconditionally re-stamps the scratch register that
 * watchdog_enable_caused_reboot() reads, and main.c calls watchdog_enable()
 * on every boot before any console command runs - so a live call from
 * plat_last_reset() would always read true after any non-power-on reset.
 * Latch it once, right after boot, before watchdog_enable() clobbers it. */
static bool s_watchdog_enable_caused_reboot;
void plat_reset_latch(void) { s_watchdog_enable_caused_reboot = watchdog_enable_caused_reboot(); }

void plat_reboot(bool bootsel) {
    if (bootsel) reset_usb_boot(0, 0);
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

void plat_halt(bool assert_halt) { gpio_put(PIN_HALT, assert_halt); }   /* HIGH = Q2 on = /HALT low */

void plat_smoke(void) {
    /* Toggle only GP0..GP7 (D0..D7) and the LED at 10 Hz for 2.5 s. A0..A13
       and R/W are 74LVC245 outputs on the real board (driven from the CoCo
       side); driving them from the Pico would contend with those buffers.
       GP26..28 (OE_BUS in, HALT, E) are left alone too: core1 polls OE_BUS
       continuously once launched, and driving it low here would fake a
       cart cycle. The console refuses this command outright while bus
       drive is on. */
    for (int t = 0; t < 50; t++) {
        for (int g = 0; g <= 7; g++) { gpio_set_dir(g, GPIO_OUT); gpio_put(g, t & 1); }
#ifdef PIN_LED
        gpio_put(PIN_LED, t & 1);   /* the main loop's own blink is stalled while smoke runs */
#endif
        sleep_ms(50); tud_task();
    }
    for (int g = 0; g <= 7; g++) gpio_set_dir(g, GPIO_IN);
}

void plat_crash_test(void) {
    ((void (*)(void))0xFFFFFFF1)();
}

const char *plat_last_reset(void) {
    static char buf[96];
    const crash_rec_t *c = crash_last();
    if (c) {
        const char *what = c->reason == CRASH_REASON_PANIC ? "panic" : "hardfault";
        snprintf(buf, sizeof(buf), "%s pc=0x%08x lr=0x%08x cfsr=0x%08x mode=%u up=%ums",
                 what, (unsigned)c->pc, (unsigned)c->lr, (unsigned)c->cfsr,
                 (unsigned)c->mode, (unsigned)c->uptime_ms);
        return buf;
    }
    /* s_watchdog_enable_caused_reboot (latched pre-boot, see plat_reset_latch)
     * is true only for a real timeout after watchdog_enable(); a commanded
     * watchdog_reboot() (our "reboot" console command, and the crash path
     * above before it stamps a record) clears that marker, so it falls
     * through to watchdog_caused_reboot() alone here and reads as "reboot". */
    if (s_watchdog_enable_caused_reboot) return "watchdog";
    return watchdog_caused_reboot() ? "reboot" : "power-on";
}

size_t plat_bridge_read(uint8_t *buf, size_t n) { return tud_cdc_n_connected(0) ? tud_cdc_n_read(0, buf, n) : 0; }

size_t plat_bridge_write_free(void) { return tud_cdc_n_connected(0) ? tud_cdc_n_write_available(0) : 0; }

size_t plat_bridge_write(const uint8_t *buf, size_t n) {
    size_t w = tud_cdc_n_write(0, buf, n);
    tud_cdc_n_write_flush(0);
    return w;
}

/* Filesystem and config: FatFS on the flash partition mounted by fs_flash.c. */
int plat_fs_list(void (*cb)(const char *, uint32_t, void *), void *ctx) {
    DIR dir;
    FILINFO fno;
    if (f_opendir(&dir, "/") != FR_OK) return -1;
    for (;;) {
        if (f_readdir(&dir, &fno) != FR_OK) { f_closedir(&dir); return -1; }
        if (fno.fname[0] == '\0') break;   /* end of directory */
        if ((fno.fattrib & AM_DIR) || fno.fname[0] == '.') continue;
        cb(fno.fname, (uint32_t)fno.fsize, ctx);
    }
    f_closedir(&dir);
    return 0;
}
int plat_fs_remove(const char *n) { return f_unlink(n) == FR_OK ? 0 : -1; }
/* Reformatting while a drive is mounted leaves its dw_disk pointed at a
 * dangling FIL on the old volume; the console's "fs format" checks
 * g_dw->drives[] and refuses if anything is still mounted. */
int plat_fs_format(void) { return fs_flash_format(); }
int plat_fs_export(bool on) {
    usb_msc_clear_ejected();
    return fs_flash_export(on);
}
bool plat_fs_exporting(void) { return fs_flash_exporting(); }
bool plat_usb_ejected(void) { return usb_msc_ejected(); }
int plat_cfg_read(char *b, size_t m) {
    static FIL f; /* static: too big for the 4 KB core0 stack */
    if (f_open(&f, "picoco.cfg", FA_READ) != FR_OK) return -1;
    UINT n = 0;
    FRESULT r = f_read(&f, b, (UINT)m, &n);
    f_close(&f);
    return r == FR_OK ? (int)n : -1;
}
int plat_cfg_write(const char *b, size_t n) {
    static FIL f; /* static: too big for the 4 KB core0 stack */
    if (f_open(&f, "picoco.cfg", FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return -1;
    UINT written = 0;
    FRESULT r = f_write(&f, b, (UINT)n, &written);
    f_close(&f);
    return (r == FR_OK && written == n) ? 0 : -1;
}
const char *plat_fs_dir(void) { return ""; }
void plat_host_set_dir(const char *d) { (void)d; }

/* RP2350 always-on timer: survives a watchdog/RUN reset, not a power cycle. */
bool plat_rtc_get(int64_t *unix_secs) {
    struct timespec ts;
    if (!aon_timer_is_running() || !aon_timer_get_time(&ts)) return false;
    *unix_secs = ts.tv_sec;
    return true;
}
void plat_rtc_set(int64_t unix_secs) {
    struct timespec ts = { .tv_sec = (time_t)unix_secs, .tv_nsec = 0 };
    if (aon_timer_is_running()) aon_timer_set_time(&ts);
    else aon_timer_start(&ts);
}
