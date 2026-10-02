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

/* core1 reloads the D0-D7 output latch while idle (bus_core1.c), and this
 * test parks its write data in that same latch: let core1 finish the reload
 * that follows the previous cycle before putting data there. */
static inline void settle(void) { busy_wait_us(2); }
/* Pico 2: a write is normally followed by a settle too, so the functional
 * checks do not depend on how long core1's post-write work (hooks, write
 * ring, trace) takes; the response_after_write sweep turns that off. The old
 * loop passed "write $FF40 = 1, read back 1" without it only because the
 * written byte was still on the pads when the PIO sampled. */
static bool raw_writes __attribute__((unused));

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
    if (!rd) { settle(); sio_hw->gpio_clr = D_MASK; sio_hw->gpio_set = data; sio_hw->gpio_oe_set = D_MASK; }
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
    if (!rd) { settle(); sio_hw->gpio_clr = D_MASK; sio_hw->gpio_set = data; sio_hw->gpio_oe_set = D_MASK; }
    uint32_t w0 = (addr & 0x3FFF) | (rd ? 0x4000u : 0);
    uint32_t w1 = (sel ? 1u : 0) | ((uint32_t)delay << 1);
    pio_sm_put_blocking(pio, sm, w0);
    pio_sm_put_blocking(pio, sm, w1);
    uint32_t got = pio_sm_get_blocking(pio, sm);
    if (!rd) { sio_hw->gpio_oe_clr = D_MASK; if (!raw_writes) settle(); }
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

static bool s_drive_was;
int fake6809_begin(void) {
    uint32_t c0 = bus_stats.cycles;
    sleep_ms(100);
    if (bus_stats.cycles != c0) return -2;
    if (!pins_idle_10ms()) return -2;
    if (start() < 0) return -1;
    s_drive_was = bus_drive_get();
    bus_drive_set(true);
    { uint16_t di; uint8_t dd; while (bus_pop_write(&di, &dd)) { } }   /* pin-takeover blip, see fake6809_selftest */
    return 0;
}
uint8_t fake6809_cycle(uint16_t addr, bool rd, uint8_t data) { return cycle(addr, rd, true, data, SAMPLE_LATE); }
void fake6809_end(void) { bus_drive_set(s_drive_was); stop(); }

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

    /* Taking the pins over can blip OE_BUS once (seen on a bare Pico 2: one
     * stray idx-0 cycle ~130 us before the first real one, R/W random). It
     * lands before the baselines below, but a stray write would sit at the
     * head of the write ring and fail write_data_captured, so drain it. */
    { uint16_t di; uint8_t dd; while (bus_pop_write(&di, &dd)) { } }
    uint32_t cyc0 = bus_stats.cycles, wr0 = bus_stats.writes, ov0 = bus_stats.write_overrun;
    int fails = 0;
    #define CHECK(name, cond) do { bool ok_ = (cond); r->cycles++; if (!ok_) { fails++; r->mismatches++; } \
        snprintf(buf, sizeof buf, "selftest %s %s", name, ok_ ? "ok" : "FAIL"); line(buf); } while (0)

    CHECK("read_bank0_marker", cycle(0xC000, true, true, 0, SAMPLE_LATE) == 0xA5);
    CHECK("read_bank0_fill",   cycle(0xC001, true, true, 0, SAMPLE_LATE) == 0x00);
    CHECK("read_top_of_window", cycle(0xFEFF, true, true, 0, SAMPLE_LATE) == 0x00);
#ifdef PICOCO_PIO_ENGINE
    (void)cyc0; (void)wr0;
    /* Pico 2 PIO engine spike: no write path, no hooks, no counters. */
    #define SKIP(name) line("selftest " name " skipped (spike)")
    SKIP("bank_switch_next_read"); SKIP("bank_switch_back");
    SKIP("becker_status_read");    /* banked: the engine serves the I/O page from the bank buffer */
    SKIP("writes_counted"); SKIP("unselected_ignored"); SKIP("write_data_captured");
    SKIP("no_ring_overrun"); SKIP("cycles_counted");
    #undef SKIP
#else
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
            uint16_t widx = 0xFFFF; uint8_t wdata = 0;
            bool have = bus_pop_write(&widx, &wdata);
            if (!have || widx != exp_idx[i] || wdata != exp_data[i]) {
                if (ok) {   /* report the first mismatch only */
                    snprintf(buf, sizeof buf, "selftest write %u: got %s%04x/%02x want %04x/%02x",
                             (unsigned)i, have ? "" : "(empty) ", widx, wdata, exp_idx[i], exp_data[i]);
                    line(buf);
                }
                ok = false;
            }
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
#endif
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
    /* The sweep above runs its cycles back to back, so core1 is still in its
     * post-cycle work when OE_BUS falls: this is the no-idle-sample path
     * (late_precompute), the one a CPU executing from the cart ROM takes. Its
     * budget is the 0.89 MHz one (E high ~560 ns), same 480 ns as the Plus-W
     * burst check; bench 2026-10-01 measured 256-322 ns. */
    CHECK("response_within_480ns", r->first_ok_delay >= 0 && r->delay_ns <= 480);
#ifndef PICOCO_BOARD_PLUSW
    /* Same sweep with core1 idle before each cycle: the path a Becker poll
     * takes (precomputed latch, one output-enable store). 32 tries per delay
     * so the PIO/core1 phase wanders across the idle pass. Budget at 1.79 MHz:
     * 279 ns of E high less ~40 ns setup and U10. Best case for that path:
     * the PIO gives ~265 ns of address setup, longer than a recompute pass, so
     * the latch is always loaded; a real 1.79 MHz CPU gives less and a poll
     * can land mid-recompute, a few tens of ns later than this figure. */
    int idle_ok = -1;
    for (int d = 0; d <= 60 && idle_ok < 0; d++) {
        int bad = 0;
        for (int i = 0; i < 32; i++) {
            cycle(0xC001, true, true, 0, 60);      /* a 0x00 read first: pads start low whatever the loop does after a cycle */
            busy_wait_us(3);
            if (cycle(0xC000, true, true, 0, (uint8_t)d) != 0xA5) bad++;
        }
        if (!bad) idle_ok = d;
    }
    /* A read straight after a write (CPU executing from the cart ROM stores
     * to $FF4x, then fetches): core1 is still in bus_on_write. 0.89 MHz budget. */
    int aw_ok = -1;
    raw_writes = true;
    for (int d = 0; d <= 120 && aw_ok < 0; d += 2) {
        int bad = 0;
        for (int i = 0; i < 8; i++) {
            busy_wait_us(3);
            cycle(0xFF40, false, true, 0, SAMPLE_LATE);
            if (cycle(0xC000, true, true, 0, (uint8_t)d) != 0xA5) bad++;
        }
        { uint16_t di; uint8_t dd; while (bus_pop_write(&di, &dd)) { } }   /* keep the write ring from filling */
        if (!bad) aw_ok = d;
    }
    raw_writes = false;
    unsigned aw_ns = aw_ok < 0 ? 0 : (unsigned)((aw_ok + 2) * ns_per);
    snprintf(buf, sizeof buf, "selftest response_after_write first_ok_delay %d (~%u ns after OE_BUS fell)", aw_ok, aw_ns);
    line(buf);
    CHECK("response_after_write_within_480ns", aw_ok >= 0 && aw_ns <= 480);
    unsigned idle_ns = idle_ok < 0 ? 0 : (unsigned)((idle_ok + 2) * ns_per);
    snprintf(buf, sizeof buf, "selftest response_idle first_ok_delay %d (~%u ns after OE_BUS fell)", idle_ok, idle_ns);
    line(buf);
    CHECK("response_idle_within_230ns", idle_ok >= 0 && idle_ns <= 230);
#endif
#ifndef PICOCO_PIO_ENGINE   /* D0-D7 belong to PIO0 there: SIO cannot drive them */
    {   /* RP2350-E9 probe, information only: drive D0-D7 high, release onto the
         * internal pull-downs and see how long they still read high. A working
         * pull-down (~50-80 k into a few pF) is gone in microseconds. */
        sio_hw->gpio_set = D_MASK; sio_hw->gpio_oe_set = D_MASK; busy_wait_us(2); sio_hw->gpio_oe_clr = D_MASK;
        busy_wait_us(10);  unsigned h10 = sio_hw->gpio_in & D_MASK;
        busy_wait_us(990); unsigned h1m = sio_hw->gpio_in & D_MASK;
        sleep_ms(50);      unsigned h50 = sio_hw->gpio_in & D_MASK;
        sio_hw->gpio_clr = D_MASK; sio_hw->gpio_oe_set = D_MASK; busy_wait_us(2); sio_hw->gpio_oe_clr = D_MASK;
        busy_wait_us(10);  unsigned l10 = sio_hw->gpio_in & D_MASK;
        snprintf(buf, sizeof buf, "selftest pad_hold high then released: 10us %02x 1ms %02x 51ms %02x; low then released: %02x", h10, h1m, h50, l10);
        line(buf);
    }
#endif
    r->ring_overrun = bus_stats.write_overrun - ov0;
    #undef CHECK

    rom_off();
    bus_drive_set(drive_was);
    stop();
    line("selftest rom cleared; reload with rom load");
    return fails ? -1 : 0;
}

#ifndef PICOCO_BOARD_PLUSW
/* `bus selftest fast`: real back-to-back 1.79 MHz cycles. fake6809_fast (PIO1,
 * clkdiv 1) is fed one word per cycle by DMA and its samples are captured by
 * DMA, so nothing on core0 can stretch a cycle. Timing is in fake6809.pio. */
#include "hardware/dma.h"

#define FAST_N 4096
#define FAST_UNSEL (1u << 17)            /* TX word: OE_BUS stays high this cycle */
#define FAST_WIN 0x3F00                  /* ROM window: table indices 0x0000-0x3EFF */
static uint32_t fast_tx[FAST_N], fast_rx[FAST_N];
static uint8_t  stress_buf[4096];

static uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

/* Table fill and address walk: every byte nonzero (an undriven bus reads 0x00)
 * and every cycle's byte differs from the previous cycle's. */
static void fast_pattern(void) {
    uint32_t s = 0x1234567u;
    for (uint32_t i = 0; i < FAST_WIN; i++) { uint8_t b = (uint8_t)xs32(&s); bus_table[i] = b ? b : 0x5A; }
    uint16_t prev = 0;
    for (int k = 0; k < FAST_N; k++) {
        uint16_t idx;
        do idx = (uint16_t)(xs32(&s) % FAST_WIN); while (bus_peek(idx) == bus_peek(prev));
        fast_tx[k] = idx | 0x4000u;      /* R/W high: read */
        prev = idx;
    }
}

/* every third cycle a CoCo write (R/W low) when on */
static void fast_mix(bool on) {
    for (int k = 0; k < FAST_N; k++) fast_tx[k] = (on && k % 3 == 2) ? (fast_tx[k] & ~0x4000u) : (fast_tx[k] | 0x4000u);
}
/* k%4: 0 read, 1 unselected, 2 read, 3 write: covers write->read, read->gap,
 * gap->read and write->read->gap. Off: all selected reads. */
static void fast_gaps(bool on) {
    for (int k = 0; k < FAST_N; k++) {
        uint32_t w = (fast_tx[k] | 0x4000u) & ~FAST_UNSEL;
        if (on && k % 4 == 1) w |= FAST_UNSEL;
        if (on && k % 4 == 3) w &= ~0x4000u;
        fast_tx[k] = w;
    }
}

static void fast_timing(int e, int pre, int post) {
    for (int k = 0; k < FAST_N; k++)
        fast_tx[k] = (fast_tx[k] & (0x7FFFu | FAST_UNSEL)) | ((uint32_t)e << 15) | ((uint32_t)pre << 18) | ((uint32_t)post << 25);
}

typedef struct { uint32_t mism, zero, stale, rel_bad, lost, spur, wrong, restarts; int first_k; uint16_t first_idx; uint8_t first_exp, first_got; } fast_res_t;

static int fdma_tx = -1, fdma_rx = -1;
static bool fast_stress, fast_restart;

static void fast_burst(fast_res_t *r) {
    memset(r, 0, sizeof *r);
    r->first_k = -1;
    for (int k = 0; k < FAST_N; k++) fast_rx[k] = 0xDEADBEEFu;
    dma_channel_config c = dma_channel_get_default_config(fdma_rx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, false));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(fdma_rx, &c, fast_rx, &pio->rxf[sm], FAST_N, false);
    c = dma_channel_get_default_config(fdma_tx);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(pio, sm, true));
    channel_config_set_high_priority(&c, true);
    dma_channel_configure(fdma_tx, &c, &pio->txf[sm], fast_tx, FAST_N, false);
    dma_start_channel_mask((1u << fdma_rx) | (1u << fdma_tx));
    uint32_t t0 = time_us_32();
    uint32_t next_rs = t0 + 50;
    while (dma_channel_is_busy(fdma_rx) && time_us_32() - t0 < 20000) {
        if (fast_stress) memcpy(stress_buf, stress_buf + 2048, 2048), memcpy(stress_buf + 2048, stress_buf, 2048);
#ifdef PICOCO_PIO_ENGINE
        /* restart the engine under a running burst: it comes back mid-cycle */
        if (fast_restart && (int32_t)(time_us_32() - next_rs) >= 0) { bus_engine_rebase(); r->restarts++; next_rs += 50; }
#endif
    }
    if (dma_channel_is_busy(fdma_rx)) {          /* a dropped push (RX full) leaves the count short */
        r->lost = dma_channel_hw_addr(fdma_rx)->transfer_count;
        dma_channel_abort(fdma_rx);
        dma_channel_abort(fdma_tx);
    }
    uint8_t prev = 0;
    for (int k = 0; k < FAST_N; k++) {
        uint16_t idx = fast_tx[k] & 0x3FFF;
        bool drv = bus_drive && (fast_tx[k] & 0x4000u) && !(fast_tx[k] & FAST_UNSEL);   /* a write or bus drive off: the pads must stay undriven (0x00) */
        uint8_t exp = drv ? bus_peek(idx) : 0, got = (uint8_t)(fast_rx[k] >> 8), rel = (uint8_t)fast_rx[k];
        if (rel) r->rel_bad++;
        if (got != exp) {
            r->mism++;
            if (got == 0) r->zero++; else if (got == prev) r->stale++;
            if (!drv) r->spur++; else if (got) r->wrong++;
            if (r->first_k < 0) { r->first_k = k; r->first_idx = idx; r->first_exp = exp; r->first_got = got; }
        }
        prev = exp;
    }
}

/* Patch the release-check point: `rel` nop delay k, first pull delay 13-k. */
static void fast_set_rel(int k) {
    uint16_t p = fake6809_fast_program_instructions[0], n = fake6809_fast_program_instructions[fake6809_fast_offset_relchk];
    pio->instr_mem[offset] = (p & ~0x0F00u) | (uint16_t)((13 - k) << 8);
    pio->instr_mem[offset + fake6809_fast_offset_relchk] = (n & ~0x0F00u) | (uint16_t)(k << 8);
}

static void fast_line(void (*line)(const char *), const char *tag, int s, const fast_res_t *r) {
    char buf[160];
    snprintf(buf, sizeof buf, "fast %s S=%d (%u ns): mismatches %lu/%d (zero %lu stale %lu spurious %lu wrong %lu) rel_nonzero %lu lost %lu%s",
             tag, s, (unsigned)(s * 20 / 3), (unsigned long)r->mism, FAST_N, (unsigned long)r->zero,
             (unsigned long)r->stale, (unsigned long)r->spur, (unsigned long)r->wrong,
             (unsigned long)r->rel_bad, (unsigned long)r->lost, r->restarts ? " (restarts)" : "");
    line(buf);
    if (r->first_k >= 0) {
        snprintf(buf, sizeof buf, "fast %s first bad cycle %d idx %04x want %02x got %02x",
                 tag, r->first_k, r->first_idx, r->first_exp, r->first_got);
        line(buf);
    }
}

/* Sweep the sample point S = 1..smax at one clock rate; print per-S mismatch
 * counts (S = 1..lowsum+1) and return response_clk: the smallest S from which every later S reads
 * the whole burst correctly (-1: never). */
static int fast_sweep(void (*line)(const char *), const char *tag, int e, int lowsum) {
    char buf[200];
    int pos = snprintf(buf, sizeof buf, "fast %s sweep", tag), stable = -1, inbuf = 0;
    fast_res_t r;
    for (int s = 1; s <= lowsum + 1; s++) {
        fast_timing(e, s - 1, lowsum - (s - 1));
        fast_burst(&r);
        if (r.mism == 0) { if (stable < 0) stable = s; } else stable = -1;
        pos += snprintf(buf + pos, sizeof buf - pos, " %d:%lu", s, (unsigned long)r.mism);
        inbuf = 1;
        if (pos > 150) { line(buf); pos = snprintf(buf, sizeof buf, "fast %s sweep", tag); inbuf = 0; }
    }
    if (inbuf) line(buf);
    snprintf(buf, sizeof buf, "fast %s response_clk %d (%d ns after OE_BUS fell)", tag, stable, stable < 0 ? -1 : stable * 20 / 3);
    line(buf);
    return stable;
}

int fake6809_fast(bool stress, void (*line)(const char *s)) {
    char buf[128];
    uint32_t c0 = bus_stats.cycles;
    sleep_ms(100);
    if (bus_stats.cycles != c0) return -2;
    if (!pins_idle_10ms()) return -2;
    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) return -1;
    offset = pio_add_program(pio, &fake6809_fast_program);
    pio_sm_config c = fake6809_fast_program_get_default_config(offset);
    sm_config_set_out_pins(&c, PIN_A0, 15);
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_sideset_pins(&c, PIN_OE_BUS);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pins_to_pio();
    uint32_t bypass_was = pio->input_sync_bypass;
    pio->input_sync_bypass = bypass_was | D_MASK;   /* sample D0-D7 at the stated clk, not 2 clk later */
    pio_sm_init(pio, sm, offset, &c);
    fdma_tx = dma_claim_unused_channel(true);
    fdma_rx = dma_claim_unused_channel(true);
    fast_stress = stress;
    bool drive_was = bus_drive_get();
    bus_drive_set(true);
    rom_banks_begin();                               /* unbanked: the window is bus_table */
    fast_pattern();
    fast_set_rel(3);
    pio_sm_set_enabled(pio, sm, true);
#ifdef PICOCO_PIO_ENGINE
    {   /* What an 8-bit DMA write puts in a 32-bit TX FIFO: PIO2 SM0, never started. */
        PIO p2 = pio2;
        pio_sm_claim(p2, 0);
        pio_sm_clear_fifos(p2, 0);
        static uint8_t probe = 0xA7;
        int ch = dma_claim_unused_channel(true);
        dma_channel_config dc = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, false);
        dma_channel_configure(ch, &dc, &p2->txf[0], &probe, 1, true);
        dma_channel_wait_for_finish_blocking(ch);
        dma_channel_unclaim(ch);
        pio_sm_exec(p2, 0, pio_encode_pull(false, true));
        pio_sm_exec(p2, 0, pio_encode_mov(pio_isr, pio_osr));
        pio_sm_exec(p2, 0, pio_encode_push(false, true));
        snprintf(buf, sizeof buf, "fast dma 8-bit write of %02x reaches the TX FIFO as %08lx", probe, (unsigned long)p2->rxf[0]);
        line(buf);
        pio_sm_unclaim(p2, 0);
        bool byp, pri;
        bus_engine_get(&byp, &pri);
        snprintf(buf, sizeof buf, "fast engine: PIO0 + DMA, input sync bypass %s, DMA bus priority %s, %s", byp ? "on" : "off", pri ? "on" : "off", bus_engine_desc());
        line(buf);
    }
#endif
    snprintf(buf, sizeof buf, "fast timing: cycle %d clk (high 42, low 42), address setup 25 clk, %d cycles back to back%s",
             84, FAST_N, stress ? ", core0 memcpy stress" : "");
    line(buf);

    fast_res_t r;
    int rc = 0;
    for (int i = 0; i < 5; i++) {                    /* (a) realistic: S=36 (240 ns) at 1.79 MHz */
        fast_timing(0, 35, 39 - 35);
        fast_burst(&r);
        fast_line(line, "1.79MHz", 36, &r);
        if (r.mism || r.lost) rc = -1;
    }
    fast_mix(true);                                  /* reads with writes between them: writes must never be driven */
    fast_timing(0, 35, 4);
    fast_burst(&r);
    fast_line(line, "1.79MHz mixed r/w", 36, &r);
    if (r.mism || r.lost) rc = -1;
    fast_mix(false);
    fast_gaps(true);                                 /* unselected gaps: never driven, no stale trigger */
    fast_burst(&r);
    fast_line(line, "1.79MHz gaps r/w/unsel", 36, &r);
    if (r.mism || r.lost) rc = -1;
#ifdef PICOCO_PIO_ENGINE
    fast_restart = true;                             /* engine restarted every 50 us under the burst */
    fast_burst(&r);
    fast_restart = false;
    snprintf(buf, sizeof buf, "fast 1.79MHz restarts %lu under a gaps burst: spurious %lu wrong %lu (zero %lu = cycles lost to a restart)",
             (unsigned long)r.restarts, (unsigned long)r.spur, (unsigned long)r.wrong, (unsigned long)r.zero);
    line(buf);
    if (r.spur || r.wrong || r.lost) rc = -1;
    fast_burst(&r);
    fast_line(line, "1.79MHz gaps after restarts", 36, &r);
    if (r.mism || r.lost) rc = -1;
#endif
    fast_gaps(false);
    bus_drive_set(false);                            /* capture-only: nothing may be driven */
    fast_burst(&r);
    fast_line(line, "1.79MHz drive off", 36, &r);
    if (r.mism || r.lost) rc = -1;
    bus_drive_set(true);
    int resp = fast_sweep(line, "1.79MHz", 0, 39);   /* (b) */
    if (resp < 0 || resp > 36) rc = -1;
    {   /* release: earliest point after the rise where D0-D7 read 0 for the whole burst */
        int rel_ok = -1;
        fast_timing(0, 35, 4);
        for (int k = 0; k <= 13 && rel_ok < 0; k++) { fast_set_rel(k); fast_burst(&r); if (!r.rel_bad && !r.mism) rel_ok = k + 1; }
        fast_set_rel(3);
        snprintf(buf, sizeof buf, "fast 1.79MHz release_clk %d (D0-D7 low on every cycle %d clk after OE_BUS rose; -1 = not by 14)", rel_ok, rel_ok);
        line(buf);
    }
    fast_timing(3, 71, 81 - 71);                     /* (c) 0.89 MHz: high 84, low 84, S=72 (480 ns) */
    fast_burst(&r);
    fast_line(line, "0.89MHz", 72, &r);
    if (r.mism || r.lost) rc = -1;
    fast_sweep(line, "0.89MHz", 3, 81);

    pio_sm_set_enabled(pio, sm, false);
    dma_channel_unclaim(fdma_tx);
    dma_channel_unclaim(fdma_rx);
    pio->input_sync_bypass = bypass_was;
    pins_to_sio();
    pio_remove_program(pio, &fake6809_fast_program, offset);
    pio_sm_unclaim(pio, sm);
    sm = -1;
    rom_off();
    bus_drive_set(drive_was);
    line("selftest rom cleared; reload with rom load");
    return rc;
}
#endif
