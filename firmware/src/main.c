#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include <string.h>
#include "tusb.h"
#include PICOCO_BOARD_H
#include "bus.h"
#include "device.h"
#include "rom.h"
#include "becker.h"
#include "dw.h"
#include "dw_store.h"
#include "log.h"
#include "console.h"
#include "mode.h"
#include "plat.h"
#include "fs_flash.h"
#include "crash.h"

static dw_server g_dw;
static dw_store  g_store;

static uint32_t mode_get_u32(void) { return (uint32_t)mode_get(); }

static void console_out(void *ctx, const char *s) {
    (void)ctx;
    if (!tud_cdc_n_connected(1)) return;              /* drop when nobody is listening */
    size_t len = strlen(s), off = 0;
    while (off < len) {
        uint32_t avail = tud_cdc_n_write_available(1);
        if (avail == 0) {
            /* ponytail: bounded 50 ms wait for the host to drain its FIFO
             * (status/stats/trace dump can exceed the 512-byte TX buffer in
             * one console_feed call); past the deadline drop the rest rather
             * than block the main loop indefinitely. */
            uint32_t deadline = plat_now_ms() + 50;
            // ponytail: re-enters tud_task(); safe only while no TinyUSB callback prints or logs-with-drain
            do { tud_task(); watchdog_update(); avail = tud_cdc_n_write_available(1); }
            while (avail == 0 && plat_now_ms() < deadline);
            if (avail == 0) break;
        }
        uint32_t chunk = avail < (uint32_t)(len - off) ? avail : (uint32_t)(len - off);
        off += tud_cdc_n_write(1, s + off, chunk);
        tud_cdc_n_write_flush(1);
    }
}
static void gpio_setup(void) {
    for (int g = 0; g <= 28; g++) {
        if (g >= 23 && g <= 25) continue;             /* internal on the module */
        gpio_init(g); gpio_set_dir(g, GPIO_IN); gpio_pull_up(g);
    }
    gpio_init(PIN_HALT); gpio_set_dir(PIN_HALT, GPIO_OUT); gpio_put(PIN_HALT, 1);   /* keep /HALT asserted until released after core1 launch, below */
    gpio_init(PIN_LED);  gpio_set_dir(PIN_LED, GPIO_OUT);
}
int main(void) {
    gpio_setup();
    tusb_init();
    log_init();
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
    dw_store_fatfs_init(&g_store);
    dw_init(&g_dw, &g_store, mode_dw_send, NULL);
    console_init(console_out, NULL, &g_dw, &g_store);
    crash_init();
    crash_mode_hook = mode_get_u32;
    plat_reset_latch();   /* before watchdog_enable() below clobbers the marker it reads */
    watchdog_enable(8000, true);
    LOG_I(LOG_M_MAIN, "boot");
    if (fs_flash_mount() == 0) {
        int n = console_run_config();
        if (n < 0) LOG_I(LOG_M_MAIN, "fs ok, config: none");
        else LOG_I(LOG_M_MAIN, "fs ok, config lines %d", n);
    } else {
        LOG_E(LOG_M_FS, "fs mount failed");
    }
    multicore_launch_core1(bus_core1_main);
    gpio_put(PIN_HALT, 0);   /* release /HALT: spec 8.1 */
    LOG_I(LOG_M_MAIN, "core1 up, halt released");
    uint32_t last_blink = 0; bool led = false;
    for (;;) {
        tud_task();
        uint32_t now = plat_now_ms();
        mode_pump(&g_dw, now);
        if (tud_cdc_n_available(1)) { uint8_t b[64]; uint32_t n = tud_cdc_n_read(1, b, sizeof b); console_feed(b, n); }
        if (tud_cdc_n_connected(1)) {
            /* Only drain (i.e. pop) log lines with a host attached, so boot
             * lines survive a reboot's reconnect gap for "log dump". */
            char lb[256]; size_t ln = log_drain(lb, sizeof lb - 1);
            if (ln) { lb[ln] = 0; console_out(NULL, lb); }
        }
        if (fs_flash_exporting()) {
            gpio_put(PIN_LED, 1);   /* solid while the USB drive is exported */
        } else {
            uint32_t period = mode_get() == MODE_NATIVE ? 250 : 500;  /* 2 Hz native, 1 Hz otherwise */
            if (now - last_blink >= period) { last_blink = now; led = !led; gpio_put(PIN_LED, led); }
        }
        watchdog_update();
    }
}
