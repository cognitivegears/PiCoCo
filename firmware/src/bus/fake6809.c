#include "fake6809.h"
#include "bus.h"
#include "rom.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "pico/stdlib.h"
#include "fake6809.pio.h"
#include PICOCO_BOARD_H
#include <stdio.h>
#include <string.h>

#define CLKDIV 1.1f
#define D_MASK 0xFFu

static PIO pio = pio1;
static int sm = -1;
static uint offset;

#ifdef PICOCO_BOARD_PLUSW
#define PW_BIT(gp) (1u << ((gp) - PIN_A0))
static uint32_t phase(uint16_t addr, bool rd, bool sel, bool e, bool q) {
    uint32_t w = (addr & 0x3FFF) | (rd ? PW_BIT(PIN_RW) : 0) | PW_BIT(PIN_SLENB)
               | ((addr & 0x4000) ? PW_BIT(PIN_A14) : 0) | ((addr & 0x8000) ? PW_BIT(PIN_A15) : 0)
               | (e ? PW_BIT(PIN_E) : 0) | (q ? PW_BIT(PIN_Q) : 0);
    /* hardware select: /CTS for the ROM window, /SCS for $FF40-$FF5F; firmware-decoded addresses assert neither */
    bool cts = sel && addr < 0xFF00, scs = sel && (addr & 0xFFE0) == 0xFF40;
    if (!cts) w |= PW_BIT(PIN_CTS);
    if (!scs) w |= PW_BIT(PIN_SCS);
    return w;
}
static void pins_to_pio(void) {
    uint32_t idle = phase(0, true, false, false, false);
    pio_sm_set_pins_with_mask(pio, sm, idle << PIN_A0, 0x7FFFFFu << PIN_A0);
    for (int g = PIN_A0; g <= PIN_A15; g++) pio_gpio_init(pio, g);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 23, true);
}
static void pins_to_sio(void) {
    for (int g = PIN_A0; g <= PIN_A15; g++) { gpio_set_function(g, GPIO_FUNC_SIO); gpio_set_dir(g, GPIO_IN); gpio_pull_up(g); }
#ifdef PIN_LED
    gpio_set_dir(PIN_LED, GPIO_OUT);
#endif
}
/* One bus cycle: six TX words (P0, P1, P2, sample-delay control, P3, P4—
 * see fake6809.pio's fake6809_pw header). For writes, D0..D7 are driven from
 * core0's SIO for the duration (core1 only samples them on a write). E high
 * lasts P2..P4, about 32 + delay + 34 PIO cycles at clkdiv 1.1. Returns the
 * byte the PIO sampled during E high (a read's data), or 0 if unselected. */
static uint8_t cycle(uint16_t addr, bool rd, bool sel, uint8_t data, uint8_t delay) {
    if (!rd) { sio_hw->gpio_clr = D_MASK; sio_hw->gpio_set = data; sio_hw->gpio_oe_set = D_MASK; }
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, false));   /* P0 */
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, true));    /* P1 */
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, true, true));     /* P2 */
    pio_sm_put_blocking(pio, sm, delay);
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, true, false));    /* P3 */
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, false));   /* P4: E=0 Q=0, cycle over */
    uint32_t got = pio_sm_get_blocking(pio, sm);
    if (!rd) sio_hw->gpio_oe_clr = D_MASK;
    return (uint8_t)(got & 0xFF);
}
#define PROGRAM fake6809_pw_program
#define CONFIG  fake6809_pw_program_get_default_config
#define OUT_COUNT 23
#else
static void pins_to_pio(void) {
    /* OE_BUS must read high before the SM takes the pin, or core1 sees a fake cycle. */
    pio_sm_set_pins_with_mask(pio, sm, 1u << PIN_OE_BUS, 1u << PIN_OE_BUS);
    for (int g = PIN_A0; g <= PIN_RW; g++) pio_gpio_init(pio, g);
    pio_gpio_init(pio, PIN_OE_BUS);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, 15, true);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_OE_BUS, 1, true);
}

static void pins_to_sio(void) {
    for (int g = PIN_A0; g <= PIN_RW; g++) { gpio_set_function(g, GPIO_FUNC_SIO); gpio_set_dir(g, GPIO_IN); gpio_pull_up(g); }
    gpio_set_function(PIN_OE_BUS, GPIO_FUNC_SIO); gpio_set_dir(PIN_OE_BUS, GPIO_IN); gpio_pull_up(PIN_OE_BUS);
}

/* One bus cycle. For writes, D0..D7 are driven from core0's SIO for the
 * duration (core1 only samples them on a write). Returns the byte the PIO
 * sampled during OE low (a read's data), or 0 for an unselected cycle. */
static uint8_t cycle(uint16_t addr, bool rd, bool sel, uint8_t data, uint8_t delay) {
    if (!rd) { sio_hw->gpio_clr = D_MASK; sio_hw->gpio_set = data; sio_hw->gpio_oe_set = D_MASK; }
    uint32_t w0 = (addr & 0x3FFF) | (rd ? 0x4000u : 0);
    uint32_t w1 = (sel ? 1u : 0) | ((uint32_t)delay << 1);
    pio_sm_put_blocking(pio, sm, w0);
    pio_sm_put_blocking(pio, sm, w1);
    uint32_t got = pio_sm_get_blocking(pio, sm);
    if (!rd) sio_hw->gpio_oe_clr = D_MASK;
    return (uint8_t)(got & 0xFF);
}
#define PROGRAM fake6809_p2_program
#define CONFIG  fake6809_p2_program_get_default_config
#define OUT_COUNT 15
#endif

static int start(void) {
    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) return -1;
    offset = pio_add_program(pio, &PROGRAM);
    pio_sm_config c = CONFIG(offset);
    sm_config_set_out_pins(&c, PIN_A0, OUT_COUNT);
    sm_config_set_in_pins(&c, PIN_D0);
#ifndef PICOCO_BOARD_PLUSW
    sm_config_set_sideset_pins(&c, PIN_OE_BUS);
#endif
    sm_config_set_out_shift(&c, true, false, 32);    /* shift right: bit 0 first */
    sm_config_set_in_shift(&c, false, false, 32);    /* shift left: one in pins,8 leaves the byte in bits 0-7 */
    sm_config_set_clkdiv(&c, CLKDIV);
    pins_to_pio();
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
    return 0;
}

static void stop(void) {
    pio_sm_set_enabled(pio, sm, false);
    pins_to_sio();
    pio_remove_program(pio, &PROGRAM, offset);
    pio_sm_unclaim(pio, sm);
    sm = -1;
}

#define SAMPLE_LATE 40   /* delay used for functional checks: well after core1 has driven data */

static uint8_t img[2 * ROM_BANK_SIZE];

int fake6809_selftest(fake_result_t *r, void (*line)(const char *s)) {
    char buf[96];
    memset(r, 0, sizeof *r);
    r->first_ok_delay = -1;

    uint32_t c0 = bus_stats.cycles;
    sleep_ms(100);
    if (bus_stats.cycles != c0) return -2;                 /* a CoCo is driving the bus: refuse */
    if (start() < 0) return -1;

    bool drive_was = bus_drive_get();
    bus_drive_set(true);
    /* Synthetic 32 KB banked image: bank b is filled with b, byte 0 marked. */
    for (int b = 0; b < 2; b++) memset(img + b * ROM_BANK_SIZE, b, ROM_BANK_SIZE);
    img[0] = 0xA5;
    rom_load_mem(img, sizeof img);

    uint32_t cyc0 = bus_stats.cycles, wr0 = bus_stats.writes, ov0 = bus_stats.write_overrun;
    int fails = 0;
    #define CHECK(name, cond) do { bool ok_ = (cond); r->cycles++; if (!ok_) { fails++; r->mismatches++; } \
        snprintf(buf, sizeof buf, "selftest %s %s", name, ok_ ? "ok" : "FAIL"); line(buf); } while (0)

    CHECK("read_bank0_marker", cycle(0xC000, true, true, 0, SAMPLE_LATE) == 0xA5);
    CHECK("read_bank0_fill",   cycle(0xC001, true, true, 0, SAMPLE_LATE) == 0x00);
    CHECK("read_top_of_window", cycle(0xFEFF, true, true, 0, SAMPLE_LATE) == 0x00);
    cycle(0xFF40, false, true, 1, SAMPLE_LATE);            /* bank select 1 */
    CHECK("bank_switch_next_read", cycle(0xC001, true, true, 0, SAMPLE_LATE) == 0x01);
    cycle(0xFF40, false, true, 0, SAMPLE_LATE);
    CHECK("bank_switch_back", cycle(0xC001, true, true, 0, SAMPLE_LATE) == 0x00);
    CHECK("becker_status_read", cycle(0xFF41, true, true, 0, SAMPLE_LATE) == 0x00);
    for (int i = 0; i < 10; i++) cycle(0xFF42, false, true, (uint8_t)i, SAMPLE_LATE);
    CHECK("writes_counted", bus_stats.writes - wr0 == 10 + 2);
    uint32_t before = bus_stats.cycles;
    cycle(0xC001, true, false, 0, SAMPLE_LATE);            /* unselected: core1 must not see it */
    CHECK("unselected_ignored", bus_stats.cycles == before);
#ifdef PICOCO_BOARD_PLUSW
    uint32_t fw0 = bus_stats.fw_selected, cyc_fw = bus_stats.cycles;
    cycle(0xFF7E, true, false, 0, SAMPLE_LATE);            /* not enabled: ignored */
    CHECK("fw_unenabled_ignored", bus_stats.cycles == cyc_fw);
    bus_fw_enable(0xFF7E);
    bus_set_read(0x3F7E, 0x9F);
    CHECK("fw_read", cycle(0xFF7E, true, false, 0, SAMPLE_LATE) == 0x9F);
    uint32_t wr_fw = bus_stats.writes;
    cycle(0xFF7E, false, false, 0x42, SAMPLE_LATE);
    CHECK("fw_write_queued", bus_stats.writes == wr_fw + 1 && bus_stats.fw_selected == fw0 + 2);
    CHECK("fw_other_page_ignored", (cycle(0xBF7E, true, false, 0, SAMPLE_LATE), bus_stats.fw_selected == fw0 + 2));
    bus_fw_disable(0xFF7E);
    bus_set_read(0x3F7E, 0xFF);
#endif
    CHECK("no_ring_overrun", bus_stats.write_overrun == ov0);
#ifdef PICOCO_BOARD_PLUSW
#define CYCLES_EXPECTED (8 + 10 + 2)   /* + fw_read + fw_write_queued */
#else
#define CYCLES_EXPECTED (8 + 10)
#endif
    CHECK("cycles_counted", bus_stats.cycles - cyc0 == CYCLES_EXPECTED);   /* 6 reads + 2 bank writes + 10 $FF42 writes; the unselected cycle is not seen */
#undef CYCLES_EXPECTED

    /* Timing sweep: smallest sample delay at which core1's read data is already valid. */
    for (int d = 0; d <= 60; d++) {
        if (cycle(0xC001, true, true, 0, (uint8_t)d) == 0x00 && cycle(0xC000, true, true, 0, (uint8_t)d) == 0xA5) {
            r->first_ok_delay = d;
            break;
        }
    }
    float ns_per = 1e9f * CLKDIV / (float)clock_get_hz(clk_sys);
    r->delay_ns = r->first_ok_delay < 0 ? 0 : (uint32_t)((r->first_ok_delay + 2) * ns_per);   /* +2: nop + first jmp before the sample */
    snprintf(buf, sizeof buf, "selftest response first_ok_delay %d (~%u ns after OE_BUS fell)", r->first_ok_delay, (unsigned)r->delay_ns);
    line(buf);
    CHECK("response_measured", r->first_ok_delay >= 0);
    r->ring_overrun = bus_stats.write_overrun - ov0;
    #undef CHECK

    rom_off();
    bus_drive_set(drive_was);
    stop();
    line("selftest rom cleared; reload with rom load");
    return fails ? -1 : 0;
}
