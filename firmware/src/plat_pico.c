#include "plat.h"
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include PICOCO_BOARD_H

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

/* Filesystem and config: Task 3. Export: Task 4. */
int plat_fs_list(void (*cb)(const char *, uint32_t, void *), void *ctx) { (void)cb; (void)ctx; return -1; }
int plat_fs_remove(const char *n) { (void)n; return -1; }
int plat_fs_format(void) { return -1; }
int plat_fs_export(bool on) { (void)on; return -1; }
int plat_cfg_read(char *b, size_t m) { (void)b; (void)m; return -1; }
int plat_cfg_write(const char *b, size_t n) { (void)b; (void)n; return -1; }
const char *plat_fs_dir(void) { return ""; }
void plat_host_set_dir(const char *d) { (void)d; }
