#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include <string.h>
#include "tusb.h"
#include PICOCO_BOARD_H
#include "bus.h"
#include "bus_engine.h"
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
#include "net.h"

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
    for (int g = 0; g < 48; g++) {
        if (PICOCO_INPUT_MASK & (1ULL << g)) { gpio_init(g); gpio_set_dir(g, GPIO_IN); gpio_pull_up(g); }
    /* D0-D7 idle LOW, not high: U10 is enabled by hardware (E-qualified /OE) from the
     * start of every cart read, so whatever the Pico's data pads hold before core1
     * drives them is what the CoCo sees if the drive lands late. Pulled up that was
     * 0xFF, whose bit 1 reads as Becker "data ready": one late $FF41 poll made the
     * CoCo read a phantom byte and desynced the whole stream (PCB bench 2026-09-30,
     * 1.79 MHz). Pulled down a late poll reads 0x00, not ready, and is retried. */
    for (int g = PIN_D0; g < PIN_D0 + 8; g++) gpio_pull_down(g);
    }
    gpio_init(PIN_HALT); gpio_put(PIN_HALT, 1); gpio_set_dir(PIN_HALT, GPIO_OUT);   /* latch the value before enabling the output so /HALT never glitches low; keep it asserted until released after core1 launch, below */
#ifdef PIN_CART_DRV
    gpio_init(PIN_CART_DRV); gpio_set_dir(PIN_CART_DRV, GPIO_OUT); gpio_put(PIN_CART_DRV, 0);   /* keep /CART released (Q4 off) */
#endif
#ifdef PIN_OE_FW
    gpio_init(PIN_OE_FW); gpio_put(PIN_OE_FW, 1); gpio_set_dir(PIN_OE_FW, GPIO_OUT);   /* buffer disabled until core1 selects a cycle */
#endif
#ifdef PIN_LED
    gpio_init(PIN_LED);  gpio_set_dir(PIN_LED, GPIO_OUT);
#endif
}
int main(void) {
    gpio_setup();
    bool dbl_reset = plat_double_reset();
    tusb_init();
    log_init();
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
    dw_store_fatfs_init(&g_store);
    dw_init(&g_dw, &g_store, mode_dw_send, NULL);
    console_init(console_out, NULL, &g_dw, &g_store);
    net_init();
    bus_engine_init();   /* after net_init: the cyw43 driver claims PIO2 (GPIO base 16) first */
    /* core1 straight after the event stream starts, so no event waits unread
     * through the config replay and the net hold (more than 2048 would lap
     * the ring). Every hook is registered above; the config's rom load and
     * bank changes take the engine lock (rom.c). /HALT stays held until below. */
    multicore_launch_core1(bus_core1_main);
#ifdef PICOCO_HAVE_NET
    net_dw = &g_dw;
#endif
    crash_init();
    crash_mode_hook = mode_get_u32;
    plat_reset_latch();   /* before watchdog_enable() below clobbers the marker it reads */
    watchdog_enable(8000, true);
    LOG_I(LOG_M_MAIN, "boot, reset log %u", (unsigned)plat_double_reset_seen());
    if (fs_flash_mount() == 0) {
        int n = console_run_config();
        if (n < 0) LOG_I(LOG_M_MAIN, "fs ok, config: none");
        else LOG_I(LOG_M_MAIN, "fs ok, config lines %d", n);
    } else {
        LOG_E(LOG_M_FS, "fs mount failed");
    }
    if (dbl_reset) {
        console_boot_manager();
        LOG_I(LOG_M_MAIN, "double reset: manager for this boot");
    }
    /* becker net in the config: hold /HALT (already asserted by the boot
     * pull-up) until the server socket is up, at most NET_BOOT_HOLD_MS, so a
     * CoCo never sees a half-connected board. Fall back to native otherwise:
     * the saved config still says net and the next boot tries again. */
    if (mode_get() == MODE_NET) {
        uint32_t t0 = plat_now_ms();
        while (net_state() != NET_UP && plat_now_ms() - t0 < NET_BOOT_HOLD_MS) {
            plat_double_reset_tick(plat_now_ms());
            tud_task();
            net_poll(plat_now_ms());
            watchdog_update();
        }
        if (net_state() == NET_UP) {
            LOG_I(LOG_M_MAIN, "net up in %u ms", (unsigned)(plat_now_ms() - t0));
        } else {
            LOG_I(LOG_M_MAIN, "net failed (%s), native fallback", net_last_error()[0] ? net_last_error() : "timeout");
            mode_set(MODE_NATIVE);
            console_set_boot_mode(MODE_NET);   /* keep the saved intent */
        }
    }
    int64_t rtc;
    if (plat_rtc_get(&rtc)) dw_time_set(&g_dw, rtc, plat_now_ms());
    gpio_put(PIN_HALT, 0);   /* release /HALT: spec 8.1 */
    LOG_I(LOG_M_MAIN, "core1 up, halt released, bus cycles %u", (unsigned)bus_stats.cycles);   /* nonzero: selects during the hold */
#ifdef PIN_CART_DRV
    /* Autostart paks expect /CART pulsing after reset (a real pak ties it to
     * Q). Toggle Q4 for 500 ms after the /HALT release so Color BASIC's
     * cart check sees an edge after it has initialised the PIA; DOS ROMs
     * ("DK") never get this, they would jump to $C000 as code. /RESET drives
     * the Pico's RUN pin through U13/R9 (hardware-design.md §4.5), so a CoCo
     * reset-button press reboots the Pico too and repeats this whole boot
     * sequence, pulse included — this is not power-on/Pico-reboot only. */
    /* Not after a double RESET: the manager is a DK image, and /CART would send BASIC to $C000. */
    uint32_t cart_until = (rom_cart_wanted() && !dbl_reset) ? plat_now_ms() + 500 : 0;
#endif
#ifdef PIN_LED
    uint32_t last_blink = 0; bool led = false;
#endif
    for (;;) {
        tud_task();
        uint32_t now = plat_now_ms();
        plat_double_reset_tick(now);
        mode_pump(&g_dw, now);
        bus_engine_tick(now);
        net_poll(now);
#ifdef PIN_CART_DRV
        if (cart_until) {
            if (now < cart_until) gpio_put(PIN_CART_DRV, now & 1);
            else { gpio_put(PIN_CART_DRV, 0); cart_until = 0; }
        }
#endif
        if (tud_cdc_n_available(1)) { uint8_t b[64]; uint32_t n = tud_cdc_n_read(1, b, sizeof b); console_feed(b, n); }
        if (tud_cdc_n_connected(1)) {
            /* Only drain (i.e. pop) log lines with a host attached, so boot
             * lines survive a reboot's reconnect gap for "log dump". */
            char lb[256]; size_t ln = log_drain(lb, sizeof lb - 1);
            if (ln) { lb[ln] = 0; console_out(NULL, lb); }
        }
#ifdef PIN_LED
        if (fs_flash_exporting()) {
            gpio_put(PIN_LED, 1);   /* solid while the USB drive is exported */
        } else {
            uint32_t period = mode_get() == MODE_NATIVE || mode_get() == MODE_NET ? 250 : 500;  /* 2 Hz native, 1 Hz otherwise */
            if (now - last_blink >= period) { last_blink = now; led = !led; gpio_put(PIN_LED, led); }
        }
#endif
        watchdog_update();
    }
}
