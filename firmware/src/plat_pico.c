#include "plat.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include PICOCO_BOARD_H
#include "fs_flash.h"
#include "usb_descriptors.h"
#include "ff.h"

uint32_t plat_now_us(void) { return time_us_32(); }
uint32_t plat_now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

void plat_reboot(bool bootsel) {
    if (bootsel) reset_usb_boot(0, 0);
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

void plat_halt(bool assert_halt) { gpio_put(PIN_HALT, assert_halt); }   /* HIGH = Q2 on = /HALT low */

void plat_smoke(void) {
    /* Toggle every header GPIO at 10 Hz for 5 s; core1 is not running yet in Task 2.
       ponytail: after Task 5 this must only run with bus drive off; the console enforces it. */
    for (int t = 0; t < 50; t++) {
        for (int g = 0; g <= 28; g++) if (g < 23 || g > 25) { gpio_set_dir(g, GPIO_OUT); gpio_put(g, t & 1); }
        gpio_put(PIN_LED, t & 1);   /* the main loop's own blink is stalled while smoke runs */
        sleep_ms(50); tud_task();
    }
    /* PIN_HALT must come back out as an asserted output, not an input: leaving
       it floating/input would silently disable plat_halt() until reboot. */
    for (int g = 0; g <= 28; g++) if ((g < 23 || g > 25) && g != PIN_HALT) gpio_set_dir(g, GPIO_IN);
    gpio_set_dir(PIN_HALT, GPIO_OUT);
    gpio_put(PIN_HALT, 1);
}

size_t plat_bridge_read(uint8_t *buf, size_t n) { return tud_cdc_n_connected(0) ? tud_cdc_n_read(0, buf, n) : 0; }

size_t plat_bridge_write(const uint8_t *buf, size_t n) {
    /* ponytail: report the full count "written" when nobody is listening so
       mode_pump's flow control doesn't spin retrying bytes into the void. */
    if (!tud_cdc_n_connected(0)) return n;
    size_t w = tud_cdc_n_write(0, buf, n);
    tud_cdc_n_write_flush(0);
    return w;
}

/* Filesystem and config: FatFS on the flash partition mounted by fs_flash.c.
 * Export (USB MSC): Task 4. */
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
    if (on) usb_msc_clear_ejected();
    return fs_flash_export(on);
}
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
