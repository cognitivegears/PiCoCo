#include "pico/stdlib.h"
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

static dw_server g_dw;
static dw_store  g_store;

/* ponytail: no storage backend yet (Task 3 replaces this with FatFS). Every
 * op fails cleanly instead of leaving g_store->ops NULL, so dw_disk_open,
 * rom_load_file and the console's capture/rom-load paths (which call
 * store->ops->* unguarded) get an error return rather than a NULL deref. */
static int none_open(void *ctx, const char *name, bool write, dw_file *f) {
    (void)ctx; (void)name; (void)write; (void)f; return -1;
}
static int none_create(void *ctx, const char *name, dw_file *f) {
    (void)ctx; (void)name; (void)f; return -1;
}
static int none_read(dw_file *f, uint32_t off, void *buf, uint32_t n) {
    (void)f; (void)off; (void)buf; (void)n; return -1;
}
static int none_write(dw_file *f, uint32_t off, const void *buf, uint32_t n) {
    (void)f; (void)off; (void)buf; (void)n; return -1;
}
static int none_size(dw_file *f, uint32_t *out) { (void)f; (void)out; return -1; }
static int none_sync(dw_file *f) { (void)f; return -1; }
static void none_close(dw_file *f) { (void)f; }
static const dw_store_ops none_ops = {
    .open = none_open, .create = none_create, .read = none_read,
    .write = none_write, .size = none_size, .sync = none_sync, .close = none_close,
};

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
    gpio_init(PIN_HALT); gpio_set_dir(PIN_HALT, GPIO_OUT); gpio_put(PIN_HALT, 1);   /* keep /HALT asserted until Task 5 releases it */
    gpio_init(PIN_LED);  gpio_set_dir(PIN_LED, GPIO_OUT);
}
int main(void) {
    gpio_setup();
    tusb_init();
    log_init();
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
    g_store.ops = &none_ops;
    g_store.ctx = NULL;
    dw_init(&g_dw, &g_store, mode_dw_send, NULL);
    console_init(console_out, NULL, &g_dw, &g_store);
    watchdog_enable(8000, true);
    LOG_I(LOG_M_MAIN, "boot");
    /* Task 3: mount fs, console_run_config().  Task 5: crash_report(), core1 launch, halt release. */
    uint32_t last_blink = 0; bool led = false;
    for (;;) {
        tud_task();
        uint32_t now = plat_now_ms();
        mode_pump(&g_dw, now);
        if (tud_cdc_n_available(1)) { uint8_t b[64]; uint32_t n = tud_cdc_n_read(1, b, sizeof b); console_feed(b, n); }
        char lb[256]; size_t ln = log_drain(lb, sizeof lb - 1);
        if (ln) { lb[ln] = 0; console_out(NULL, lb); }
        uint32_t period = mode_get() == MODE_NATIVE ? 250 : 500;      /* 2 Hz native, 1 Hz otherwise */
        if (now - last_blink >= period) { last_blink = now; led = !led; gpio_put(PIN_LED, led); }
        watchdog_update();
    }
}
