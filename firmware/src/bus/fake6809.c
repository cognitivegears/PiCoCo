#include "fake6809.h"
#include "bus.h"
#include "rom.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
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
/* push_cycle/pop_cycle split the six-word cycle so a burst can keep two PIO
 * cycles in flight (push cycle k+2 while popping cycle k) with no gap for a
 * real back-to-back bus cycle to be skipped in. D0-7 SIO handling for
 * writes stays in cycle() only; bursts (burst_reads, below) are reads, so
 * push_cycle never needs to touch D0-7. */
static void push_cycle(uint16_t addr, bool rd, bool sel, uint8_t data, uint8_t delay) {
    (void)data;
    uint32_t s = save_and_disable_interrupts();   /* a core0 IRQ between TX words must not stretch a phase */
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, false));   /* P0 */
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, true));    /* P1 */
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, true, true));     /* P2 */
    pio_sm_put_blocking(pio, sm, delay);
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, true, false));    /* P3 */
    pio_sm_put_blocking(pio, sm, phase(addr, rd, sel, false, false));   /* P4: E=0 Q=0, cycle over */
    restore_interrupts(s);
}
static uint8_t pop_cycle(void) {
    return (uint8_t)(pio_sm_get_blocking(pio, sm) & 0xFF);
}
static uint8_t cycle(uint16_t addr, bool rd, bool sel, uint8_t data, uint8_t delay) {
    if (!rd) { sio_hw->gpio_clr = D_MASK; sio_hw->gpio_set = data; sio_hw->gpio_oe_set = D_MASK; }
    push_cycle(addr, rd, sel, data, delay);
    uint8_t got = pop_cycle();
    if (!rd) sio_hw->gpio_oe_clr = D_MASK;
    return got;
}
/* Keeps two cycles queued the whole way through so no real E/Q edge between
 * them could be missed; returns the mismatch count against expect[]. */
static uint32_t burst_reads(const uint16_t *addrs, const uint8_t *expect, int n, uint8_t delay) {
    uint32_t mismatches = 0;
    if (n <= 0) return 0;
    push_cycle(addrs[0], true, true, 0, delay);
    if (n > 1) push_cycle(addrs[1], true, true, 0, delay);
    for (int k = 0; k < n; k++) {
        if (pop_cycle() != expect[k]) mismatches++;
        if (k + 2 < n) push_cycle(addrs[k + 2], true, true, 0, delay);
    }
    return mismatches;
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

/* Raw pin-activity guard for fake6809_selftest: an idle CoCo runs from RAM
 * and produces no cart cycles, so bus_stats.cycles never moves and the
 * cycle-counter check below passes even with the board plugged in. A0-A13
 * come through always-enabled buffers and toggle whenever the CPU runs
 * though; unplugged, the pins sit on their pull-ups and don't move. This is
 * a backstop, not a licence — unplug the board before running this. */
#ifdef PICOCO_BOARD_PLUSW
#define ACTIVITY_MASK ((0x3FFFu << PIN_A0) | (1u << PIN_RW) | (1u << PIN_E) | (1u << PIN_Q))
#else
#define ACTIVITY_MASK ((0x3FFFu << PIN_A0) | (1u << PIN_RW) | (1u << PIN_OE_BUS))
#endif
static bool pins_idle_10ms(void) {
    uint32_t first = sio_hw->gpio_in & ACTIVITY_MASK;
    uint32_t deadline = time_us_32() + 10000;
    while ((int32_t)(time_us_32() - deadline) < 0) {
        sleep_us(50);
        if ((sio_hw->gpio_in & ACTIVITY_MASK) != first) return false;
    }
    return true;
}

int fake6809_selftest(fake_result_t *r, void (*line)(const char *s)) {
    char buf[96];
    memset(r, 0, sizeof *r);
    r->first_ok_delay = -1;
    r->burst_first_ok_delay = -1;

    uint32_t c0 = bus_stats.cycles;
    sleep_ms(100);
    if (bus_stats.cycles != c0) return -2;                 /* a CoCo is driving the bus: refuse */
    if (!pins_idle_10ms()) return -2;                       /* idle CoCo: no cart cycles, but pins still move */
    if (start() < 0) return -1;

    bool drive_was = bus_drive_get();
    bus_drive_set(true);
    /* Synthetic 32 KB banked image, built straight in rom_banks (no 32 KB
     * local staging copy): bank 0 filled with 0x00 and byte 0 marked, bank
     * 1 filled with 0x01. */
    rom_banks_begin();
    memset(rom_bank_buf(0), 0x00, ROM_BANK_SIZE);
    rom_bank_buf(0)[0] = 0xA5;
    memset(rom_bank_buf(1), 0x01, ROM_BANK_SIZE);
    rom_publish_banks(2);

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
    busy_wait_us(2);   /* core1 finishes bus_on_* a few hundred ns after the PIO has already pushed the cycle's result */
    CHECK("writes_counted", bus_stats.writes - wr0 == 10 + 2);
    uint32_t before = bus_stats.cycles;
    cycle(0xC001, true, false, 0, SAMPLE_LATE);            /* unselected: core1 must not see it */
    busy_wait_us(2);
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
    busy_wait_us(2);
    CHECK("fw_write_queued", bus_stats.writes == wr_fw + 1 && bus_stats.fw_selected == fw0 + 2);
    CHECK("fw_other_page_ignored", (cycle(0xBF7E, true, false, 0, SAMPLE_LATE), bus_stats.fw_selected == fw0 + 2));
    bus_fw_disable(0xFF7E);
    bus_set_read(0x3F7E, 0xFF);
#endif
    /* Drain the write ring now and check every captured event, in order:
     * the two bank-select writes, the ten $FF42 writes, and (Plus-W) the
     * one firmware-decoded write above. Also stops these bytes leaking
     * into the DriveWire server later (becker_refresh polls the same ring). */
    {
        static const uint16_t exp_idx[] = {
            0x3F40, 0x3F40, 0x3F42, 0x3F42, 0x3F42, 0x3F42, 0x3F42, 0x3F42, 0x3F42, 0x3F42, 0x3F42, 0x3F42,
#ifdef PICOCO_BOARD_PLUSW
            0x3F7E,
#endif
        };
        static const uint8_t exp_data[] = {
            1, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
#ifdef PICOCO_BOARD_PLUSW
            0x42,
#endif
        };
        busy_wait_us(2);
        bool ok = true;
        for (size_t i = 0; i < sizeof(exp_idx) / sizeof(exp_idx[0]); i++) {
            uint16_t widx; uint8_t wdata;
            if (!bus_pop_write(&widx, &wdata) || widx != exp_idx[i] || wdata != exp_data[i]) ok = false;
        }
        CHECK("write_data_captured", ok);
    }
    busy_wait_us(2);
    CHECK("no_ring_overrun", bus_stats.write_overrun == ov0);
#ifdef PICOCO_BOARD_PLUSW
#define CYCLES_EXPECTED (8 + 10 + 2)   /* + fw_read + fw_write_queued */
#else
#define CYCLES_EXPECTED (8 + 10)
#endif
    busy_wait_us(2);
    CHECK("cycles_counted", bus_stats.cycles - cyc0 == CYCLES_EXPECTED);   /* 6 reads + 2 bank writes + 10 $FF42 writes; the unselected cycle is not seen */
#undef CYCLES_EXPECTED
    float ns_per = 1e9f * CLKDIV / (float)clock_get_hz(clk_sys);
#ifdef PICOCO_BOARD_PLUSW
    {
        /* Eight back-to-back reads, no gap: bank 0 is still selected (the
         * last switch above was back to bank 0), so C000/C001 alternate
         * the bank-0 marker/fill bytes 0xA5/0x00. Sweep the sample delay:
         * the smallest one with zero mismatches over the whole burst is how
         * late core1's post-cycle work (hook scan, trace record, stats) can
         * run before back-to-back reads start missing data. */
        static const uint16_t baddrs[8]  = { 0xC000, 0xC001, 0xC000, 0xC001, 0xC000, 0xC001, 0xC000, 0xC001 };
        static const uint8_t  bexpect[8] = { 0xA5, 0x00, 0xA5, 0x00, 0xA5, 0x00, 0xA5, 0x00 };
        uint32_t cburst = 0;
        for (int d = 0; d <= 120; d += 2) {
            busy_wait_us(2);   /* let core1 finish bus_on_read_done for the previous burst's last cycle before sampling the baseline */
            cburst = bus_stats.cycles;
            if (burst_reads(baddrs, bexpect, 8, (uint8_t)d) == 0) { r->burst_first_ok_delay = d; break; }
        }
        r->burst_delay_ns = r->burst_first_ok_delay < 0 ? 0 : (uint32_t)((r->burst_first_ok_delay + 4) * ns_per);
        snprintf(buf, sizeof buf, "selftest burst first_ok_delay %d (~%u ns after E rose)", r->burst_first_ok_delay, (unsigned)r->burst_delay_ns);
        line(buf);
        CHECK("burst_back_to_back", r->burst_first_ok_delay >= 0);
        /* A CoCo 3 gives about 560 ns of E high and the 6809 wants data ~80 ns
         * before E falls; 480 ns is the initial budget, to be revisited
         * against a scope. */
        CHECK("burst_within_480ns", r->burst_first_ok_delay >= 0 && r->burst_delay_ns <= 480);
        busy_wait_us(2);
        CHECK("burst_cycles_counted", bus_stats.cycles - cburst == 8);
    }
#endif

    /* Timing sweep: smallest sample delay at which core1's read data is already valid. */
    for (int d = 0; d <= 60; d++) {
        if (cycle(0xC001, true, true, 0, (uint8_t)d) == 0x00 && cycle(0xC000, true, true, 0, (uint8_t)d) == 0xA5) {
            r->first_ok_delay = d;
            break;
        }
    }
#ifdef PICOCO_BOARD_PLUSW
    r->delay_ns = r->first_ok_delay < 0 ? 0 : (uint32_t)((r->first_ok_delay + 4) * ns_per);   /* +4: out pins(P2) + pull + out y + first jmp before the sample, measured from E rising */
    snprintf(buf, sizeof buf, "selftest response first_ok_delay %d (~%u ns after E rose)", r->first_ok_delay, (unsigned)r->delay_ns);
#else
    r->delay_ns = r->first_ok_delay < 0 ? 0 : (uint32_t)((r->first_ok_delay + 2) * ns_per);   /* +2: nop + first jmp before the sample */
    snprintf(buf, sizeof buf, "selftest response first_ok_delay %d (~%u ns after OE_BUS fell)", r->first_ok_delay, (unsigned)r->delay_ns);
#endif
    line(buf);
    CHECK("response_measured", r->first_ok_delay >= 0);
    /* 300 ns is the initial threshold (Task 3's ~280 ns CoCo 3 budget,
     * docs/firmware-architecture.md §8); tighten once a real bench run
     * reports an actual number. */
    CHECK("response_within_300ns", r->first_ok_delay >= 0 && r->delay_ns <= 300);
    r->ring_overrun = bus_stats.write_overrun - ov0;
    #undef CHECK

    rom_off();
    bus_drive_set(drive_was);
    stop();
    line("selftest rom cleared; reload with rom load");
    return fails ? -1 : 0;
}
