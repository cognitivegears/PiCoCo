#include "fake6809.h"
#include "bus.h"
#include "bus_engine.h"
#include "rom.h"
#include "becker.h"
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

/* Fake OE_BUS for a bare module (the carrier's U15 is absent): fake_decode_pw
 * in PIO2, the GPIO-base-16 block the engine's helper and the cyw43 driver
 * share, drives GP40 from the fake 6809's E, /CTS and /SCS. */
static PIO const dpio = pio2;
static int dsm = -1;
static uint doff;
#define DEC_BYPASS (7ull << PIN_CTS)   /* GP24-26; GP40 keeps the synchroniser the engine's helper uses */
static uint64_t dec_bypass_was;
static void decode_start(void) {
    dsm = pio_claim_unused_sm(dpio, true);
    doff = pio_add_program(dpio, &fake_decode_pw_program);
    pio_sm_config c = fake_decode_pw_program_get_default_config(doff);
    sm_config_set_in_pins(&c, PIN_CTS);
    sm_config_set_in_pin_count(&c, 2);               /* mov x, pins = /SCS:/CTS only */
    sm_config_set_jmp_pin(&c, PIN_E);
    sm_config_set_set_pins(&c, PIN_OE_BUS, 1);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(dpio, (uint)dsm, doff, &c);
    pio_sm_set_pins_with_mask64(dpio, (uint)dsm, 1ull << PIN_OE_BUS, 1ull << PIN_OE_BUS);   /* high before the pad is ours */
    pio_sm_set_pindirs_with_mask64(dpio, (uint)dsm, 1ull << PIN_OE_BUS, 1ull << PIN_OE_BUS);
    pio_gpio_init(dpio, PIN_OE_BUS);
    pio_sm_exec(dpio, (uint)dsm, pio_encode_set(pio_y, 3));   /* both selects high */
    dec_bypass_was = (uint64_t)dpio->input_sync_bypass << pio_get_gpio_base(dpio);
    pio_set_input_sync_bypass_with_mask64(dpio, DEC_BYPASS, DEC_BYPASS);
    pio_sm_set_enabled(dpio, (uint)dsm, true);
}
static void decode_stop(void) {
    pio_sm_set_enabled(dpio, (uint)dsm, false);
    gpio_set_function(PIN_OE_BUS, GPIO_FUNC_SIO); gpio_set_dir(PIN_OE_BUS, GPIO_IN); gpio_pull_up(PIN_OE_BUS);
    pio_set_input_sync_bypass_with_mask64(dpio, dec_bypass_was, DEC_BYPASS);
    pio_remove_program(dpio, &fake_decode_pw_program, doff);
    pio_sm_unclaim(dpio, (uint)dsm);
    dsm = -1;
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
#ifdef PICOCO_BOARD_PLUSW
    decode_start();
#endif
    return 0;
}

static void stop(void) {
#ifdef PICOCO_BOARD_PLUSW
    decode_stop();
#endif
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
    memset(rom_bank_buf(0), 0x00, BUS_IO_LO);         /* the I/O entries are the devices' */
    rom_bank_buf(0)[0] = 0xA5;
    memset(rom_bank_buf(1), 0x01, BUS_IO_LO);
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
    (void)cyc0; (void)wr0;
    /* PIO engine spike: no write path, no hooks, no counters. */
    #define SKIP(name) line("selftest " name " skipped (spike)")
    SKIP("bank_switch_next_read"); SKIP("bank_switch_back");
    SKIP("becker_status_read");    /* banked: the engine serves the I/O page from the bank buffer */
    SKIP("writes_counted"); SKIP("unselected_ignored"); SKIP("write_data_captured");
    SKIP("no_ring_overrun"); SKIP("cycles_counted");
    #undef SKIP
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
        (void)cburst;
        line("selftest burst_cycles_counted skipped (spike)");
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

#ifdef PICOCO_PIO_ENGINE
/* `bus selftest fast`: real back-to-back 1.79 MHz cycles, DMA-fed, so nothing
 * on core0 can stretch a cycle. Pico 2: fake6809_fast (PIO1) drives OE_BUS
 * directly. Plus-W: fake6809_fast_pw (PIO1) drives E, /CTS, /SCS and
 * fake_decode_pw (PIO2) makes OE_BUS on GP40 from them, as U15 would. Timing
 * is in fake6809.pio. */
#include "hardware/dma.h"
#include "hardware/watchdog.h"
#include "hardware/structs/busctrl.h"
#include "hardware/structs/m33.h"
#include "net.h"
#ifdef PICOCO_BOARD_PLUSW
#include "pico/cyw43_arch.h"
#endif

#define FAST_N 4096
#define FAST_WIN 0x3F00                  /* ROM window: table indices 0x0000-0x3EFF */
#define FAST_IO  0x3F40                  /* /SCS window: table indices 0x3F40-0x3F5F */
/* cycle descriptor: [13:0] table index, [14] R/W (1 = read), [16:15] select:
 * 0 none, 1 /CTS (ROM window), 2 /SCS (I/O window) */
#define D_RD   (1u << 14)
#define D_SEL(d) (((d) >> 15) & 3u)
#define D_CTS  (1u << 15)
#define D_SCS  (2u << 15)
static uint32_t fast_desc[FAST_N], fast_tx[FAST_N], fast_rx[FAST_N];
static uint8_t  stress_buf[4096], io_save[32];

static uint32_t xs32(uint32_t *s) { uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }
static uint32_t pat_seed = 0x1234567u, sw_seed = 0x2545F491u;

static uint8_t desc_byte(uint32_t d) { return bus_peek((uint16_t)(d & 0x3FFF)); }

/* Table fill (ROM window, and the 32-byte /SCS window, saved for restore) and
 * an address walk: every byte nonzero (an undriven bus reads 0x00) and every
 * read's byte differs from the previous cycle's. */
static void fast_fill(void) {
    uint32_t s = 0x1234567u;
    for (uint32_t i = 0; i < FAST_WIN; i++) { uint8_t b = (uint8_t)xs32(&s); bus_table[i] = b ? b : 0x5A; }
    memcpy(io_save, &bus_table[FAST_IO], 32);
    for (uint32_t i = 0; i < 32; i++) bus_io_set((uint16_t)(FAST_IO + i), (uint8_t)(0x80 | (i * 7 + 3)));
}
/* Banks 1-7: each its own pattern (bank 0 is fast_fill's). */
static void fast_fill_banks(void) {
    for (int b = 1; b < BUS_BANKS; b++) {
        uint32_t s = 0x1234567u + 0x9E3779B9u * (uint32_t)b;
        for (uint32_t i = 0; i < FAST_WIN; i++) { uint8_t v = (uint8_t)xs32(&s); bus_mem[b][i] = v ? v : 0x5A; }
    }
}
/* $FF41/$FF42 are left out: their read hooks (becker.c) run on core1 now and
 * rewrite those entries mid-burst. Task 3 tests them on purpose. */
static bool hooked(uint32_t d) { uint32_t i = d & 0x3FFF; return i == BUS_IDX_BECKER_STATUS || i == BUS_IDX_BECKER_DATA; }
static uint32_t pick(uint32_t sel, uint8_t avoid) {
    uint32_t d;
    do d = (sel == D_SCS ? FAST_IO + xs32(&pat_seed) % 32 : xs32(&pat_seed) % FAST_WIN) | sel | D_RD;
    while (desc_byte(d) == avoid || hooked(d));
    return d;
}
/* ROM reads (or, Plus-W late /SCS bursts, I/O reads), all selected */
static void fast_pattern(uint32_t sel) {
    pat_seed = 0x1234567u;
    uint8_t prev = 0;
    for (int k = 0; k < FAST_N; k++) { fast_desc[k] = pick(sel, prev); prev = desc_byte(fast_desc[k]); }
}
/* every 8th read an I/O-page read ($FF40-$FF5F, /SCS on a Plus-W), the walk
 * kept so that no byte repeats the previous cycle's */
static void fast_add_io(void) {
    uint8_t prev = 0;
    for (int k = 0; k < FAST_N; k++) {
        if (k % 8 == 4) fast_desc[k] = pick(D_SCS, prev);
        else if (desc_byte(fast_desc[k]) == prev) fast_desc[k] = pick(D_CTS, prev);
        prev = desc_byte(fast_desc[k]);
    }
}
/* every third cycle a CoCo write (R/W low) */
static void fast_mix(void) {
    for (int k = 2; k < FAST_N; k += 3) fast_desc[k] &= ~D_RD;
}
/* k%4: 0 read, 1 unselected, 2 read, 3 write: covers write->read, read->gap,
 * gap->read and write->read->gap. Plus-W: the k%4==2 read is an I/O (/SCS)
 * read and every other write is an /SCS write. */
static void fast_gaps(void) {
    fast_pattern(D_CTS);
    uint8_t prev = 0;
    for (int k = 0; k < FAST_N; k++) {
        uint32_t d = fast_desc[k];
#ifdef PICOCO_BOARD_PLUSW
        if (k % 4 == 2) d = pick(D_SCS, prev);
        if (k % 8 == 7) d = (FAST_IO + 2) | D_SCS;
#endif
        if (k % 4 == 1) d &= ~(3u << 15);
        if (k % 4 == 3) d &= ~D_RD;
        fast_desc[k] = d;
        if ((d & D_RD) && D_SEL(d)) prev = desc_byte(d);
    }
    (void)prev;
}

#ifdef PICOCO_BOARD_PLUSW
#define FAST_PROG    fake6809_fast_pw_program
#define FAST_INSTR   fake6809_fast_pw_program_instructions
#define FAST_RELCHK  fake6809_fast_pw_offset_relchk
static int fast_L;                       /* late select: clk after E rose, 0 = with the address */
static uint32_t fast_late_sel;
/* E-high budget: pre + post = 37 - L (1.79 MHz) or 79 - L (0.89 MHz); S counts
 * from E rising: S = L + 3 + pre. */
#define S0()         (fast_L + 3)
#define SUM(e)       ((e) ? 79 - fast_L : 37 - fast_L)
static void fast_set_late(int L, uint32_t sel) {
    fast_L = L;
    fast_late_sel = L ? sel : 0;
    uint16_t er = FAST_INSTR[fake6809_fast_pw_offset_erise];
    pio->instr_mem[offset + fake6809_fast_pw_offset_erise] = (er & ~0x0F00u) | (uint16_t)((L ? L - 1 : 0) << 8);
    pio->instr_mem[offset + fake6809_fast_pw_offset_lset] = L
        ? (uint16_t)(pio_encode_set(pio_pins, sel == D_CTS ? 2 : 1) | (1u << 12))   /* side 1: E stays high */
        : FAST_INSTR[fake6809_fast_pw_offset_lset];
}
static void fast_timing(int e, int pre, int post) {
    pio->instr_mem[offset + fake6809_fast_pw_offset_eset] = (uint16_t)pio_encode_set(pio_x, (uint)e);
    for (int k = 0; k < FAST_N; k++) {
        uint32_t d = fast_desc[k], sel = d & (3u << 15);
        uint32_t w = (d & 0x7FFFu)
                   | ((fast_late_sel || sel != D_CTS) ? (1u << 16) : 0)
                   | ((fast_late_sel || sel != D_SCS) ? (1u << 17) : 0);
        fast_tx[k] = w | ((uint32_t)pre << 18) | ((uint32_t)post << 25);
    }
}
#else
#define FAST_PROG    fake6809_fast_program
#define FAST_INSTR   fake6809_fast_program_instructions
#define FAST_RELCHK  fake6809_fast_offset_relchk
#define S0()         1
#define SUM(e)       ((e) ? 81 : 39)
static void fast_timing(int e, int pre, int post) {
    for (int k = 0; k < FAST_N; k++) {
        uint32_t d = fast_desc[k];
        fast_tx[k] = (d & 0x7FFFu) | ((uint32_t)e << 15) | (D_SEL(d) ? 0 : (1u << 17)) | ((uint32_t)pre << 18) | ((uint32_t)post << 25);
    }
}
#endif
/* sample point S (clk after OE_BUS fell on a Pico 2, after E rose on a Plus-W) */
static void fast_at(int e, int s) { int pre = s - S0(); fast_timing(e, pre, SUM(e) - pre); }

typedef struct { uint32_t mism, zero, stale, rel_bad, lost, spur, wrong, restarts, scans; int first_k; uint16_t first_idx; uint8_t first_exp, first_got;
                 int spur_k; uint8_t spur_got, spur_prev; } fast_res_t;

static int fdma_tx = -1, fdma_rx = -1;

/* Writes with data (fast_wpattern): fake_wdata in PIO0 drives them. */
static uint8_t fast_wdat[FAST_N];        /* per cycle: the data of a write, else 0 */
static uint8_t fast_wlist[FAST_N];       /* the writes' data in order, fed to fake_wdata by DMA */
static int fast_nw;
static bool fast_wdrive;                 /* this burst's writes carry fast_wdat */
static uint32_t fast_wcap[FAST_N];       /* what bus_pop_write gave during the burst: idx << 8 | data */
static int fast_ncap;
static void fast_pop(void) {
    uint16_t i; uint8_t d;
    while (bus_pop_write(&i, &d)) if (fast_ncap < FAST_N) fast_wcap[fast_ncap++] = (uint32_t)i << 8 | d;
}
/* core1 has consumed every event: the ring holds only empty slots. */
static bool events_drained(void) {
    uint32_t t0 = time_us_32();
    for (;;) {
        int i = 0;
        while (i < BUS_EVENTS && bus_events[i] == BUS_EV_NONE) i++;
        if (i == BUS_EVENTS) return true;
        if (time_us_32() - t0 > 10000) return false;
    }
}
static bool fast_stress, fast_restart, fast_switch, fast_radio;
static uint32_t radio_results, radio_scans, radio_fails;
static int radio_rc __attribute__((unused));
static void (*fast_mid_burst)(void);     /* called once, ~200 us into a burst (decode probe) */
static uint32_t fast_tick;               /* nonzero: the stall guard runs on every pass, on made-up ms from here */

#ifdef PICOCO_BOARD_PLUSW
static int radio_cb(void *env, const cyw43_ev_scan_result_t *r) { (void)env; if (r) radio_results++; return 0; }
#endif

/* Bank switches under a burst (fast_switch): banks 0 and 1 alternate.
 * - Window: each switch logs the RX progress before and after it (p0, p1:
 *   cycles whose result had landed). A read before p0 must give the old bank,
 *   one after p1 the new one, one in p0..p1 either; any other byte is bad.
 * - Phase: the fake runs clk-exact from its DMA start (84 clk per cycle; a
 *   Pico 2's unselected cycle is 1 clk longer), so the DWT cycle count at the start and just
 *   before the exec store places each exec in cycle c at phase 0..83 clk (t=0
 *   = the previous cycle's OE_BUS rise, plus a constant start offset of a few
 *   clk). The exec is placed at a random phase (fast_switch_bank) and cycle
 *   c's read tallied old/new by phase: it must switch from new to old at one
 *   phase (its `in x, 3`), and cycle c+1 must read new. */
#define SW_MAX 64
#define SW_BIN 4
typedef struct { int p0, p1; uint32_t t; uint8_t bank; } sw_rec_t;
typedef struct { uint32_t switches, bad, wrong_bank, next_old, ph_old[84 / SW_BIN + 1], ph_new[84 / SW_BIN + 1]; int bad_k; uint8_t bad_got, bad_exp; } sw_res_t;
static sw_rec_t sw_log[SW_MAX];
static int sw_n, rx_pos;
static uint8_t sw_bank0;
static uint32_t sw_t0;
static sw_res_t swr;
static int rx_progress(void) {
    while (rx_pos < FAST_N && ((volatile uint32_t *)fast_rx)[rx_pos] != 0xDEADBEEFu) rx_pos++;
    return rx_pos;
}
static int sw_c;                                 /* the fake's timeline: cycle sw_c starts sw_start clk after sw_t0 */
static uint32_t sw_start;
static inline uint32_t sw_len(int c) {
#ifdef PICOCO_BOARD_PLUSW
    (void)c;
    return 84;                                   /* the selects are pins here: every cycle is the same length */
#else
    return 84 + (D_SEL(fast_desc[c]) ? 0 : 1);
#endif
}
/* The exec lands at a chosen phase of the cycle under way: inside the lock,
 * spin on the cycle counter until that phase, then exec. SRAM, so no flash
 * fetch sits between the timestamp and the store. */
static void __no_inline_not_in_flash_func(fast_switch_bank)(uint32_t target) {
    uint8_t nb = bus_bank ^ 1;
    int p0 = rx_progress();
    uint32_t irq = bus_engine_lock(), t;
    t = m33_hw->dwt_cyccnt;                      /* catch the timeline up, then aim at target in this cycle or the next */
    while (sw_c + 1 < FAST_N && t - sw_t0 - sw_start >= sw_len(sw_c)) { sw_start += sw_len(sw_c); sw_c++; }
    uint32_t deadline = sw_t0 + sw_start + target;
    if ((int32_t)(deadline - t) < 16) deadline += sw_len(sw_c);   /* too close or past: the next cycle */
    while ((int32_t)((t = m33_hw->dwt_cyccnt) - deadline) < 0) { }
    bus_engine_set_bank_locked(nb);
    bus_engine_unlock(irq);
    sw_log[sw_n++] = (sw_rec_t){ p0, rx_progress(), t, nb };
}
/* 1 for a read cycle c whose two banks differ: old (2) or new (1); 0 otherwise */
static int sw_read_bank(int c, uint8_t oldb) {
    if (c < 0 || c >= FAST_N) return 0;
    uint32_t d = fast_desc[c];
    uint16_t idx = d & 0x3FFF;
    if (!((d & D_RD) && D_SEL(d)) || idx >= FAST_WIN || bus_mem[0][idx] == bus_mem[1][idx]) return 0;
    uint8_t got = (uint8_t)(fast_rx[c] >> 8);
    return got == bus_mem[oldb][idx] ? 2 : got == bus_mem[oldb ^ 1][idx] ? 1 : 0;
}
static void score_switches(sw_res_t *s) {
    memset(s, 0, sizeof *s);
    s->bad_k = -1;
    s->switches = (uint32_t)sw_n;
    uint8_t cur = sw_bank0;
    int j = 0;
    for (int k = 0; k < FAST_N; k++) {
        while (j < sw_n && sw_log[j].p1 < k) { cur = sw_log[j].bank; j++; }   /* switch j is behind us */
        bool inwin = j < sw_n && sw_log[j].p0 <= k;
        uint32_t d = fast_desc[k];
        uint16_t idx = d & 0x3FFF;
        uint8_t got = (uint8_t)(fast_rx[k] >> 8), now = bus_mem[cur][idx], other = bus_mem[cur ^ 1][idx];
        if (!((d & D_RD) && D_SEL(d))) { if (got == 0) continue; now = 0; }
        else if (got == now) continue;
        else if (got == other) { if (!inwin) s->wrong_bank++; continue; }
        s->bad++;
        if (s->bad_k < 0) { s->bad_k = k; s->bad_got = got; s->bad_exp = now; }
    }
    /* phase of each exec */
    uint32_t start = 0;
    int c = 0;
    uint8_t oldb = sw_bank0;
    for (j = 0; j < sw_n; j++) {
        uint32_t dt = sw_log[j].t - sw_t0;
        while (c + 1 < FAST_N && dt - start >= sw_len(c)) start += sw_len(c++);
        uint32_t ph = dt - start;
        if (ph > 84) ph = 84;
        int b = sw_read_bank(c, oldb);
        if (b == 2) s->ph_old[ph / SW_BIN]++;
        if (b == 1) s->ph_new[ph / SW_BIN]++;
        if (sw_read_bank(c + 1, oldb) == 2) s->next_old++;
        oldb = sw_log[j].bank;
    }
}

static void fast_burst(fast_res_t *r) {
    memset(r, 0, sizeof *r);
    r->first_k = r->spur_k = -1;
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
    watchdog_update();
    fast_pop();                                  /* writes left from before: not this burst's */
    fast_ncap = 0;
    rx_pos = 0; sw_n = 0; sw_bank0 = bus_bank;
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
    sw_c = 0; sw_start = 0;
    sw_t0 = m33_hw->dwt_cyccnt;
    dma_start_channel_mask((1u << fdma_rx) | (1u << fdma_tx));
    uint32_t t0 = time_us_32();
    uint32_t next_rs = t0 + 50;
    bool mid_done = false;
    /* Done when the last RX word lands. Not dma_channel_is_busy(): a core
     * reading DMA registers back to back delays DMA A's write into B's
     * trigger by a clk now and then (measured: 89 of 4096 reads 1 clk late). */
    while (((volatile uint32_t *)fast_rx)[FAST_N - 1] == 0xDEADBEEFu && time_us_32() - t0 < 20000) {
        if (fast_stress) memcpy(stress_buf, stress_buf + 2048, 2048), memcpy(stress_buf + 2048, stress_buf, 2048);
        /* restart the engine under a running burst: it comes back mid-cycle */
        if (fast_restart && (int32_t)(time_us_32() - next_rs) >= 0) { bus_engine_drive(true); r->restarts++; next_rs += 50; }
        /* bank switch under a running burst, 0 <-> 1 */
        if (fast_switch && sw_n < SW_MAX && (int32_t)(time_us_32() - next_rs) >= 0) {
            /* a random phase of the 84-clk bus cycle: 50 us alone is 89.29 cycles,
             * so a burst saw only ~7 phases 12 clk apart, and which ones depended on
             * code layout (a pause race hid there through Task 1) */
            fast_switch_bank(xs32(&sw_seed) % 84);
            r->restarts++; next_rs += 50;
        }
        if (fast_mid_burst && !mid_done && time_us_32() - t0 > 200) { fast_mid_burst(); mid_done = true; }
        if (fast_tick) { busy_wait_at_least_cycles(xs32(&sw_seed) % 128); bus_engine_tick(fast_tick++); }   /* random spacing: not phase-locked to the bus */
        fast_pop();                              /* the write queue holds 255: keep it drained (and off core0's devices) */
#ifdef PICOCO_BOARD_PLUSW
        /* radio busy: keep a scan running and the cyw43 driver polled, so its
         * PIO2 SPI state machine and its DMA channels move data during the burst */
        if (fast_radio && !fast_mid_burst) {         /* not in a probe burst: a long cyw43 call there would miss the probe start */
            if (!cyw43_wifi_scan_active(&cyw43_state)) {
                cyw43_wifi_scan_options_t opt = { 0 };
                radio_rc = cyw43_wifi_scan(&cyw43_state, &opt, NULL, radio_cb);
                if (radio_rc == 0) { r->scans++; radio_scans++; } else radio_fails++;
            }
            net_poll(time_us_32() / 1000);
        }
#endif
    }
    if (dma_channel_is_busy(fdma_rx)) {          /* a dropped push (RX full) leaves the count short */
        r->lost = dma_channel_hw_addr(fdma_rx)->transfer_count;
        dma_channel_abort(fdma_rx);
        dma_channel_abort(fdma_tx);
    }
    if (!events_drained()) r->lost++;            /* core1 never caught up: shows as lost */
    fast_pop();
    if (fast_switch) {
        score_switches(&swr);
        r->mism = swr.bad + swr.wrong_bank;
        r->first_k = swr.bad_k;
        return;
    }
    uint8_t prev = 0;
    for (int k = 0; k < FAST_N; k++) {
        uint32_t d = fast_desc[k];
        uint16_t idx = d & 0x3FFF;
        bool drv = bus_drive && (d & D_RD) && D_SEL(d);   /* a write, unselected, or bus drive off: the pads must stay undriven (0x00) */
        bool wdrv = fast_wdrive && !(d & D_RD) && D_SEL(d); /* ...unless fake_wdata drives this write's data */
        uint8_t exp = drv ? bus_peek(idx) : wdrv ? fast_wdat[k] : 0, got = (uint8_t)(fast_rx[k] >> 8), rel = (uint8_t)fast_rx[k];
        if (rel) r->rel_bad++;
        if (got != exp) {
            r->mism++;
            if (got == 0) r->zero++; else if (got == prev) r->stale++;
            if (!drv && !wdrv) { if (r->spur_k < 0) { r->spur_k = k; r->spur_got = got; r->spur_prev = prev; } r->spur++; }
            else if (got) r->wrong++;
            if (r->first_k < 0) { r->first_k = k; r->first_idx = idx; r->first_exp = exp; r->first_got = got; }
        }
        prev = exp;
    }
}

/* Patch the release-check point: `relchk` nop delay k, first pull delay 13-k. */
static void fast_set_rel(int k) {
    uint16_t p = FAST_INSTR[0], n = FAST_INSTR[FAST_RELCHK];
    pio->instr_mem[offset] = (p & ~0x0F00u) | (uint16_t)((13 - k) << 8);
    pio->instr_mem[offset + FAST_RELCHK] = (n & ~0x0F00u) | (uint16_t)(k << 8);
}

static void fast_line(void (*line)(const char *), const char *tag, int s, const fast_res_t *r) {
    char buf[200];
    snprintf(buf, sizeof buf, "fast %s S=%d (%u ns): mismatches %lu/%d (zero %lu stale %lu spurious %lu wrong %lu) rel_nonzero %lu lost %lu%s",
             tag, s, (unsigned)(s * 20 / 3), (unsigned long)r->mism, FAST_N, (unsigned long)r->zero,
             (unsigned long)r->stale, (unsigned long)r->spur, (unsigned long)r->wrong,
             (unsigned long)r->rel_bad, (unsigned long)r->lost, r->restarts ? " (restarts)" : "");
    line(buf);
#ifdef PICOCO_BOARD_PLUSW
    if (fast_radio) {
        snprintf(buf, sizeof buf, "fast %s radio: scan %s, %lu scans started so far (%lu refused, last rc %d), %lu scan results so far, net %s",
                 tag, cyw43_wifi_scan_active(&cyw43_state) ? "active" : "idle", (unsigned long)radio_scans, (unsigned long)radio_fails, radio_rc,
                 (unsigned long)radio_results, net_state_name(net_state()));
        line(buf);
    }
#endif
    if (r->first_k >= 0) {
        snprintf(buf, sizeof buf, "fast %s first bad cycle %d idx %04x want %02x got %02x",
                 tag, r->first_k, r->first_idx, r->first_exp, r->first_got);
        line(buf);
    }
}

/* Sweep the sample point over the whole high phase; print per-S mismatch
 * counts and return response_clk: the smallest S from which every later S reads
 * the whole burst correctly (-1: never). */
static int fast_sweep(void (*line)(const char *), const char *tag, int e) {
    char buf[200];
    int pos = snprintf(buf, sizeof buf, "fast %s sweep", tag), stable = -1, inbuf = 0;
    fast_res_t r;
    for (int s = S0(); s <= S0() + SUM(e); s++) {
        fast_at(e, s);
        fast_burst(&r);
        if (r.mism == 0) { if (stable < 0) stable = s; } else stable = -1;
        pos += snprintf(buf + pos, sizeof buf - pos, " %d:%lu", s, (unsigned long)r.mism);
        inbuf = 1;
        if (pos > 150) { line(buf); pos = snprintf(buf, sizeof buf, "fast %s sweep", tag); inbuf = 0; }
    }
    if (inbuf) line(buf);
#ifdef PICOCO_BOARD_PLUSW
    snprintf(buf, sizeof buf, "fast %s response_clk %d after E rose (raw; %d ns)", tag, stable, stable < 0 ? -1 : stable * 20 / 3);
#else
    snprintf(buf, sizeof buf, "fast %s response_clk %d (%d ns after OE_BUS fell)", tag, stable, stable < 0 ? -1 : stable * 20 / 3);
#endif
    line(buf);
    return stable;
}

#ifdef PICOCO_BOARD_PLUSW
/* Decode-delay probe: decode_probe_pw in PIO2 samples GP25..GP40 every clk
 * (bit 0 /SCS, bit 1 E, bit 15 OE_BUS) for 512 clk, ~6 cycles, from ~200 us
 * into a burst. The fake's own delay, E rise (or the late /SCS fall) to GP40
 * falling, as seen by one observer: that is what the corrected numbers subtract. */
#define PROBE_WORDS 256
static uint32_t probe_buf[PROBE_WORDS];
static int psm = -1, pdma = -1;
static uint poff;
static void probe_go(void) { pio_sm_set_enabled(dpio, (uint)psm, true); }
static void probe_arm(void) {
    psm = pio_claim_unused_sm(dpio, true);
    poff = pio_add_program(dpio, &decode_probe_pw_program);
    pio_sm_config c = decode_probe_pw_program_get_default_config(poff);
    sm_config_set_in_pins(&c, PIN_SCS);
    sm_config_set_in_shift(&c, true, true, 32);      /* shift right, autopush: sample 1 in [15:0], sample 2 in [31:16] */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(dpio, (uint)psm, poff, &c);
    /* both edges unsynchronised, so the probe sees pad to pad; GP40 goes back
     * to the engine's setting (synchronised) in probe_done. The helper shares
     * the bit: this probe burst's own reads are not scored. */
    pio_set_input_sync_bypass_with_mask64(dpio, ~0ull, (1ull << PIN_SCS) | (1ull << PIN_E) | (1ull << PIN_OE_BUS));
    pdma = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(pdma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, true);
    channel_config_set_dreq(&dc, pio_get_dreq(dpio, (uint)psm, false));
    channel_config_set_high_priority(&dc, true);
    dma_channel_configure(pdma, &dc, probe_buf, &dpio->rxf[psm], PROBE_WORDS, true);
    fast_mid_burst = probe_go;
}
/* min/max clk from the reference edge (E rise, or /SCS fall when late) to OE_BUS low */
static void probe_done(bool from_scs, int *dmin, int *dmax, int *n) {
    uint32_t t0 = time_us_32();
    while (dma_channel_is_busy(pdma) && time_us_32() - t0 < 10000) { }
    dma_channel_abort(pdma);                         /* never started: nothing captured, *n stays 0 */
    pio_sm_set_enabled(dpio, (uint)psm, false);
    pio_set_input_sync_bypass_with_mask64(dpio, 0, 1ull << PIN_OE_BUS);
    dma_channel_unclaim(pdma);
    pio_remove_program(dpio, &decode_probe_pw_program, poff);
    pio_sm_unclaim(dpio, (uint)psm);
    fast_mid_burst = NULL;
    *dmin = 99; *dmax = -1; *n = 0;
    int ref = -1, prev = -1;
    for (int i = 0; i < PROBE_WORDS * 2; i++) {
        uint32_t v = (probe_buf[i / 2] >> ((i & 1) * 16)) & 0xFFFF;
        bool e = v & 2, scs = v & 1, oe = v & 0x8000;
        if (prev >= 0) {
            bool pe = prev & 2, pscs = prev & 1;
            if (!from_scs && e && !pe) ref = i;
            if (from_scs && e && !scs && pscs) ref = i;
        }
        if (ref >= 0 && !oe) { int d = i - ref; if (d < *dmin) *dmin = d; if (d > *dmax) *dmax = d; (*n)++; ref = -1; }
        prev = (int)v;
    }
}
#endif

/* Writes with data: k%6 = ROM read, ROM-window write, I/O-window write, I/O
 * read, unselected, ROM read. That has read->write, two writes on
 * consecutive cycles, write->read, read->gap and gap->read. Write data is
 * nonzero (an undriven bus reads 0x00). */
static void fast_wpattern(void) {
    fast_pattern(D_CTS);
    uint32_t s = 0xC0FFEEu;
    uint8_t prev = 0;
    fast_nw = 0;
    for (int k = 0; k < FAST_N; k++) {
        uint32_t d = fast_desc[k];
        switch (k % 6) {
        case 1: d = (xs32(&s) % FAST_WIN) | D_CTS; break;
        case 2: d = (FAST_IO + xs32(&s) % 32) | D_SCS; break;
        case 3: d = pick(D_SCS, prev); break;
        case 4: d &= ~(3u << 15); break;
        default: break;
        }
        fast_wdat[k] = 0;
        if (!(d & D_RD)) { uint8_t v = (uint8_t)xs32(&s); fast_wdat[k] = v ? v : 0xA5; fast_wlist[fast_nw++] = fast_wdat[k]; }
        fast_desc[k] = d;
        if ((d & D_RD) && D_SEL(d)) prev = desc_byte(d);
    }
}

#ifdef PICOCO_BOARD_PLUSW
#define WD_PROG fake_wdata_pw_program
#define WD_CFG  fake_wdata_pw_program_get_default_config
#else
#define WD_PROG fake_wdata_p2_program
#define WD_CFG  fake_wdata_p2_program_get_default_config
#endif
static int wd_sm = -1, wd_dma = -1;
static uint wd_off;
/* fake_wdata into PIO0 for one burst, fed fast_wlist by DMA. */
static void wdata_arm(void) {
    PIO p0 = pio0;
    wd_sm = (int)pio_claim_unused_sm(p0, true);
    wd_off = pio_add_program(p0, &WD_PROG);
    pio_sm_config c = WD_CFG(wd_off);
    sm_config_set_out_pins(&c, PIN_D0, 8);
    sm_config_set_jmp_pin(&c, PIN_RW);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(p0, (uint)wd_sm, wd_off, &c);
    wd_dma = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config((uint)wd_dma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(p0, (uint)wd_sm, true));
    dma_channel_configure((uint)wd_dma, &dc, &p0->txf[wd_sm], fast_wlist, (uint)fast_nw, true);
    pio_sm_set_enabled(p0, (uint)wd_sm, true);
    fast_wdrive = true;
}
static void wdata_disarm(void) {
    pio_sm_set_enabled(pio0, (uint)wd_sm, false);
    pio0->sm[wd_sm].instr = 0xA063u;                 /* `mov pindirs, null` */
    dma_channel_abort((uint)wd_dma);
    dma_channel_unclaim((uint)wd_dma);
    pio_remove_program(pio0, &WD_PROG, wd_off);
    pio_sm_unclaim(pio0, (uint)wd_sm);
    fast_wdrive = false;
}

/* Every selected write arrived through bus_pop_write, in order, with its data.
 * Returns the writes lost, out of order or wrong (0 = all captured); line NULL:
 * no line. */
static uint32_t fast_check_writes(void (*line)(const char *), uint32_t ov0) {
    char buf[160];
    uint32_t lost = 0, order = 0, wrong = 0, n = 0;
    int p = 0;
    for (int k = 0; k < FAST_N; k++) {
        uint32_t d = fast_desc[k];
        if ((d & D_RD) || !D_SEL(d)) continue;
        n++;
        if (p >= fast_ncap) { lost++; continue; }
        uint32_t c = fast_wcap[p++];
        if ((c >> 8) != (d & 0x3FFF)) order++;
        else if ((uint8_t)c != fast_wdat[k]) wrong++;
    }
    order += (uint32_t)(fast_ncap - p);              /* more than were driven */
    lost += bus_stats.write_overrun - ov0;
    snprintf(buf, sizeof buf, "fast writes: %lu lost, %lu out of order, %lu wrong data of %lu",
             (unsigned long)lost, (unsigned long)order, (unsigned long)wrong, (unsigned long)n);
    if (line) line(buf);
    return lost + order + wrong;
}

/* The trace's last 512 events against the last 512 selected cycles driven:
 * index, R/W, data (a read: the byte served; a write: its data), and seq with
 * no gap; then the burst's event count. A word with bits 23-31 set would fail
 * the trace's tag check and show here as missing. */
static bool fast_check_events(void (*line)(const char *), uint32_t c0) {
    static bus_trace_entry te[BUS_TRACE_SIZE];
    char buf[160];
    bus_trace_freeze(true);
    size_t n = bus_trace_copy(te, BUS_TRACE_SIZE);
    bus_trace_freeze(false);
    uint32_t mism = (uint32_t)(BUS_TRACE_SIZE - n), sel = 0;
    for (int k = 0; k < FAST_N; k++) if (D_SEL(fast_desc[k])) sel++;
    int k = FAST_N;
    for (int i = (int)n - 1; i >= 0; i--) {
        do k--; while (k >= 0 && !D_SEL(fast_desc[k]));
        if (k < 0) { mism++; continue; }
        uint32_t d = fast_desc[k];
        uint16_t idx = d & 0x3FFF;
        uint8_t rd = (d & D_RD) ? 1 : 0, exp = rd ? bus_peek(idx) : fast_wdat[k];
        if (te[i].idx != idx || te[i].rw != rd || te[i].data != exp || (i && te[i].seq != te[i - 1].seq + 1)) mism++;
    }
    uint32_t counted = bus_stats.cycles - c0;
    snprintf(buf, sizeof buf, "fast events: %lu mismatches of %u (the last %u of %lu selected cycles); counted %lu of %lu",
             (unsigned long)mism, (unsigned)BUS_TRACE_SIZE, (unsigned)BUS_TRACE_SIZE, (unsigned long)sel, (unsigned long)counted, (unsigned long)sel);
    line(buf);
    return !mism && counted == sel;
}

/* Becker at speed: core0 feeds the fake's TX FIFO itself (no DMA), so the
 * next cycle can depend on an earlier read, as the CoCo's polling loop does:
 * $FF41, 4 ROM fetches, $FF42 if the status had bit 1, repeat. The decision
 * has the 4 fetches (4 cycles) to reach the FIFO. If core0 falls behind, the
 * fake's `pull block` stretches E low and sets FDEBUG.TXSTALL: counted as a
 * stall, so a pass is at speed. fast_desc[0-3] are the fetches, [4] $FF41,
 * [5] $FF42 (bk_setup); fast_tx[0-5] their words at the current timing. */
#define BK_N 2048
#define BK_S 4
#define BK_D 5
static uint8_t bk_stream[BK_N], bk_got[BK_N + 64];   /* room for a few more than asked: phantoms */
typedef struct { uint32_t cycles, polls, got, fill_bad, stalls; uint8_t last_st, last_d; } bk_res_t;
static void bk_setup(void) {
    pat_seed = 0xBECCu;
    uint8_t prev = 0;
    for (int j = 0; j < 4; j++) { fast_desc[j] = pick(D_CTS, prev); prev = desc_byte(fast_desc[j]); }
    fast_desc[BK_S] = BUS_IDX_BECKER_STATUS | D_SCS | D_RD;
    fast_desc[BK_D] = BUS_IDX_BECKER_DATA | D_SCS | D_RD;
    /* nonzero (undriven), never 0xFF (the empty port's data), and unlike its
     * two neighbours, so a lost or repeated byte is told from a phantom one */
    uint32_t s = 0xBEC4E2u;
    for (int i = 0; i < BK_N; i++) {
        uint8_t b;
        do b = (uint8_t)xs32(&s); while (b == 0 || b == 0xFF || (i > 0 && b == bk_stream[i - 1]) || (i > 1 && b == bk_stream[i - 2]));
        bk_stream[i] = b;
    }
}
/* script NULL: the client loop until n data bytes are read, with core0
 * pushing bk_stream through becker_write as the ring drains. Else the n
 * fast_desc indices in script, in order. core0 queues a byte only while the
 * TX FIFO is full (4 cycles ahead). SRAM and interrupts off: a cold XIP fetch
 * or a core0 IRQ would stall the fake (FDEBUG.TXSTALL) or overflow its 4-deep
 * RX FIFO (a dropped result, FDEBUG.RXSTALL): either counts as a stall.
 * becker_write and becker_tx_free stay in flash, warmed before the loop. */
static void __no_inline_not_in_flash_func(bk_run)(const uint8_t *script, int n, bk_res_t *r) {
    memset(r, 0, sizeof *r);
    uint8_t fexp[4], kind[16];
    for (int j = 0; j < 4; j++) fexp[j] = desc_byte(fast_desc[j]);
    uint32_t sent = 0, recv = 0, st_seq = 0, dsent = 0;
    uint32_t stall = (1u << (PIO_FDEBUG_TXSTALL_LSB + sm)) | (1u << (PIO_FDEBUG_RXSTALL_LSB + sm));
    int pos = 0, st_val = 0, w = 0, fi = 0;
    pio_sm_clear_fifos(pio, (uint)sm);
    if (!script) { w = (int)becker_write(bk_stream, BK_N); (void)becker_tx_free(); }
    watchdog_update();
    uint32_t irq = save_and_disable_interrupts(), t0 = m33_hw->dwt_cyccnt;
    for (;;) {
        while (!pio_sm_is_rx_fifo_empty(pio, (uint)sm)) {
            uint8_t b = (uint8_t)(pio->rxf[sm] >> 8), j = kind[recv & 15];
            if (j == BK_S) { r->last_st = b; if (recv == st_seq) st_val = b | 0x100; }
            else if (j == BK_D) { r->last_d = b; if (r->got < BK_N + 64) bk_got[r->got++] = b; }
            else if (b != fexp[j]) r->fill_bad++;
            recv++;
        }
        bool end = script ? pos == n : (pos == 0 && dsent == (uint32_t)n);
        if (end || m33_hw->dwt_cyccnt - t0 > 15000000u) break;   /* 100 ms */
        while (!pio_sm_is_tx_fifo_full(pio, (uint)sm) && sent - recv < 12) {
            int j;
            if (script) { if (pos == n) break; j = script[pos++]; }
            else if (pos == 0 && dsent == (uint32_t)n) break;   /* the last byte is read: no more polls */
            else if (pos == 0) { j = BK_S; st_seq = sent; st_val = 0; r->polls++; pos = 1; }
            else if (pos < 5) { j = fi++ & 3; pos++; }
            else if (!st_val) break;                     /* this group's status not back yet */
            else if (st_val & 2) { j = BK_D; dsent++; pos = 0; }
            else { pos = 0; continue; }                  /* not ready: poll again */
            kind[sent & 15] = (uint8_t)j;
            pio->txf[sm] = fast_tx[j];
            if (sent++ == 3) pio->fdebug = stall;        /* stalled until the first words: clear that */
        }
        if (!script && pio_sm_is_tx_fifo_full(pio, (uint)sm) && w < BK_N && becker_tx_free()) w += (int)becker_write(&bk_stream[w], 1);
    }
    if (sent > 4 && (pio->fdebug & stall)) r->stalls++;
    while (recv < sent && m33_hw->dwt_cyccnt - t0 < 16500000u) {   /* the cycles still in flight */
        if (pio_sm_is_rx_fifo_empty(pio, (uint)sm)) continue;
        uint8_t b = (uint8_t)(pio->rxf[sm] >> 8), j = kind[recv & 15];
        if (j == BK_S) r->last_st = b;
        else if (j == BK_D) { r->last_d = b; if (r->got < BK_N + 64) bk_got[r->got++] = b; }
        recv++;
    }
    restore_interrupts(irq);
    r->cycles = sent;
}
/* bk_got against bk_stream: lost (skipped or never read), duplicated (the
 * previous byte again), phantom (anything else). */
static void bk_score(uint32_t got, uint32_t *lost, uint32_t *dup, uint32_t *phantom) {
    uint32_t i = 0;
    *lost = *dup = *phantom = 0;
    for (uint32_t j = 0; j < got; j++) {
        uint8_t b = bk_got[j];
        if (i < BK_N && b == bk_stream[i]) i++;
        else if (i > 0 && b == bk_stream[i - 1]) (*dup)++;
        else if (i + 1 < BK_N && b == bk_stream[i + 1]) { (*lost)++; i += 2; }
        else (*phantom)++;
    }
    *lost += BK_N - i;
}
static bool bk_stream_line(void (*line)(const char *), const char *tag, const bk_res_t *r, uint32_t un0, uint32_t ov0) {
    char buf[200];
    uint32_t lost, dup, ph, un = becker_stats.underrun - un0, ov = becker_stats.overrun - ov0;
    bk_score(r->got, &lost, &dup, &ph);
    snprintf(buf, sizeof buf, "fast becker %s: %lu bytes, %lu lost, %lu duplicated, %lu phantom; underrun %lu, overrun %lu, %lu polls in %lu cycles, fetches bad %lu, stalls %lu",
             tag, (unsigned long)r->got, (unsigned long)lost, (unsigned long)dup, (unsigned long)ph, (unsigned long)un, (unsigned long)ov,
             (unsigned long)r->polls, (unsigned long)r->cycles, (unsigned long)r->fill_bad, (unsigned long)r->stalls);
    line(buf);
    return r->got == BK_N && !lost && !dup && !ph && !un && !ov && !r->fill_bad && !r->stalls;
}

/* $FF40 = n, then the same ROM address on the next three cycles (+0, +1, +2):
 * which read first gives bank n's byte. Groups of 8: the write, the three
 * reads, four unselected cycles. n is never the bank before it, and the
 * address is one where the two banks differ. */
static void fast_bank_pattern(void) {
    uint32_t s = 0xB4A1Cu;
    uint8_t cur = 0;
    fast_nw = 0;
    for (int k = 0; k < FAST_N; k += 8) {
        uint8_t n = (uint8_t)((cur + 1 + xs32(&s) % (BUS_BANKS - 1)) % BUS_BANKS);
        uint32_t a;
        do a = xs32(&s) % FAST_WIN; while (bus_mem[n][a] == bus_mem[cur][a]);
        fast_desc[k] = FAST_IO | D_SCS;              /* $FF40 */
        fast_wdat[k] = n;
        fast_wlist[fast_nw++] = n;
        for (int i = 1; i < 8; i++) { fast_desc[k + i] = a | D_RD | (i < 4 ? D_CTS : 0); fast_wdat[k + i] = 0; }
        cur = n;
    }
}
/* hist[i]: switches first seen at +i (3: not by +2); bad: a read from
 * neither bank, or the old bank after the new one. Starts from bank 0. */
static void fast_bank_score(uint32_t hist[4], uint32_t *bad) {
    uint8_t cur = 0;
    memset(hist, 0, 4 * sizeof hist[0]);
    *bad = 0;
    for (int k = 0; k < FAST_N; k += 8) {
        uint8_t n = fast_wdat[k];
        uint16_t a = fast_desc[k + 1] & 0x3FFF;
        int first = 3;
        for (int i = 0; i < 3; i++) {
            uint8_t got = (uint8_t)(fast_rx[k + 1 + i] >> 8);
            if (got == bus_mem[n][a]) { if (first == 3) first = i; }
            else if (got != bus_mem[cur][a] || first < 3) (*bad)++;
        }
        hist[first]++;
        cur = n;
    }
}

/* The $FF40 hook's latency, from core0 during a fast_bank_pattern burst
 * (fast_mid_burst): for 16 writes, clk from the write's OE_BUS rise to
 * bus_bank changing (the hook stores it just before the `set x` exec). SRAM:
 * no XIP fetch in the spins. */
static uint32_t hl_min, hl_max, hl_n;
#ifdef PICOCO_BOARD_PLUSW
#define OE_LOW() (!(sio_hw->gpio_hi_in & (1u << (PIN_OE_BUS - 32))))
#else
#define OE_LOW() (!(sio_hw->gpio_in & (1u << PIN_OE_BUS)))
#endif
static void __no_inline_not_in_flash_func(fast_hook_latency)(void) {
    uint32_t irq = save_and_disable_interrupts(), t0 = m33_hw->dwt_cyccnt, t;
    hl_min = ~0u; hl_max = 0; hl_n = 0;
    while (hl_n < 16 && m33_hw->dwt_cyccnt - t0 < 1000000) {
        if (!OE_LOW() || (sio_hw->gpio_in & (1u << PIN_RW))) continue;   /* a write's OE_BUS low */
        uint8_t b = bus_bank;
        while (OE_LOW()) { }
        t = m33_hw->dwt_cyccnt;
        while (bus_bank == b && m33_hw->dwt_cyccnt - t < 10000) { }
        t = m33_hw->dwt_cyccnt - t;
        if (t < hl_min) hl_min = t;
        if (t > hl_max) hl_max = t;
        hl_n++;
    }
    restore_interrupts(irq);
}

/* fast lap: core1 let go mid-burst (fast_mid_burst) */
static void release_hold(void) { bus_core1_hold = false; }

int fake6809_fast(int opts, void (*line)(const char *s)) {
    char buf[200];
    uint32_t c0 = bus_stats.cycles;
    sleep_ms(100);
    if (bus_stats.cycles != c0) return -2;
    if (!pins_idle_10ms()) return -2;
    sm = pio_claim_unused_sm(pio, false);
    if (sm < 0) return -1;
    offset = pio_add_program(pio, &FAST_PROG);
#ifdef PICOCO_BOARD_PLUSW
    pio_sm_config c = fake6809_fast_pw_program_get_default_config(offset);
    sm_config_set_out_pins(&c, PIN_A0, 18);          /* A0-A13, R/W, LED, /CTS, /SCS */
    sm_config_set_set_pins(&c, PIN_CTS, 2);
    sm_config_set_sideset_pins(&c, PIN_E);
    /* idle: selects high, E low, R/W high */
    pio_sm_set_pins_with_mask(pio, sm, (1u << PIN_CTS) | (1u << PIN_SCS) | (1u << PIN_RW), 0x7FFFFu << PIN_A0);
    for (int g = PIN_A0; g <= PIN_E; g++) pio_gpio_init(pio, g);
    pio_sm_set_consecutive_pindirs(pio, sm, PIN_A0, PIN_E - PIN_A0 + 1, true);
#else
    pio_sm_config c = fake6809_fast_program_get_default_config(offset);
    sm_config_set_out_pins(&c, PIN_A0, 15);
    sm_config_set_sideset_pins(&c, PIN_OE_BUS);
    pins_to_pio();
#endif
    sm_config_set_in_pins(&c, PIN_D0);
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_in_shift(&c, false, false, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    uint32_t bypass_was = pio->input_sync_bypass;
    pio->input_sync_bypass = bypass_was | D_MASK;   /* sample D0-D7 at the stated clk, not 2 clk later */
    pio_sm_init(pio, sm, offset, &c);
    fdma_tx = dma_claim_unused_channel(true);
    fdma_rx = dma_claim_unused_channel(true);
    fast_stress = opts & FAST_OPT_STRESS;
    fast_radio = opts & FAST_OPT_RADIO;
    radio_results = radio_scans = radio_fails = 0;
    bool drive_was = bus_drive_get();
    bus_drive_set(true);
    rom_banks_begin();                               /* unbanked: the window is bus_table */
    fast_fill();
    fast_pattern(D_CTS);
    fast_set_rel(3);
#ifdef PICOCO_BOARD_PLUSW
    fast_set_late(0, 0);
    decode_start();
#endif
    pio_sm_set_enabled(pio, sm, true);
    {   /* What an 8-bit DMA write puts in a 32-bit TX FIFO: an idle PIO1 SM, never started. */
        int psm8 = pio_claim_unused_sm(pio, true);
        pio_sm_clear_fifos(pio, (uint)psm8);
        static uint8_t probe8 = 0xA7;
        int ch = dma_claim_unused_channel(true);
        dma_channel_config dc = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&dc, DMA_SIZE_8);
        channel_config_set_read_increment(&dc, false);
        channel_config_set_write_increment(&dc, false);
        dma_channel_configure(ch, &dc, &pio->txf[psm8], &probe8, 1, true);
        dma_channel_wait_for_finish_blocking(ch);
        dma_channel_unclaim(ch);
        pio_sm_exec(pio, (uint)psm8, pio_encode_pull(false, true));
        pio_sm_exec(pio, (uint)psm8, pio_encode_mov(pio_isr, pio_osr));
        pio_sm_exec(pio, (uint)psm8, pio_encode_push(false, true));
        snprintf(buf, sizeof buf, "fast dma 8-bit write of %02x reaches the TX FIFO as %08lx", probe8, (unsigned long)pio->rxf[psm8]);
        line(buf);
        pio_sm_unclaim(pio, (uint)psm8);
        uint32_t byp = pio0->input_sync_bypass, am = 0x3FFFu << PIN_A0;
        const char *sy[2] = { "synchronised", "bypassed" };
        snprintf(buf, sizeof buf, "fast engine: PIO0 + DMA, pio0 input_sync_bypass %08lx: A0-A13 %s, R/W %s, GP26 %s, D0-D7 %s; DMA bus priority %s",
                 (unsigned long)byp, (byp & am) == am ? sy[1] : (byp & am) ? "mixed" : sy[0], sy[(byp >> PIN_RW) & 1],
                 sy[(byp >> 26) & 1], (byp & 0xFF) == 0xFF ? sy[1] : (byp & 0xFF) ? "mixed" : sy[0],
                 busctrl_hw->priority == (BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS) ? "on (DMA R+W)" : "other");
        line(buf);
#ifdef PICOCO_BOARD_PLUSW
        snprintf(buf, sizeof buf, "fast engine: helper OE_BUS GP40 in pio2 (gpio base %u) %s",
                 pio_get_gpio_base(pio2), sy[(pio2->input_sync_bypass >> (PIN_OE_BUS - pio_get_gpio_base(pio2))) & 1]);
        line(buf);
#endif
        snprintf(buf, sizeof buf, "fast test: fake 6809 pio1 sm%d, dma tx %d rx %d", sm, fdma_tx, fdma_rx);
        line(buf);
        bus_engine_resources(line);
    }
#ifdef PICOCO_BOARD_PLUSW
    snprintf(buf, sizeof buf, "fast timing: cycle 84 clk (E low 42, E high 42), address+selects setup 25 clk to E rise, %d cycles back to back%s%s",
             FAST_N, fast_stress ? ", core0 memcpy stress" : "", fast_radio ? ", radio busy (scans + cyw43 poll)" : "");
#else
    snprintf(buf, sizeof buf, "fast timing: cycle %d clk (high 42, low 42), address setup 25 clk, %d cycles back to back%s",
             84, FAST_N, fast_stress ? ", core0 memcpy stress" : "");
#endif
    line(buf);

    fast_res_t r;
    int rc = 0;
    {   /* event_drop's source: a nonblocking push into a full RX FIFO (idle PIO1 SM, 4 deep) */
        int psm = (int)pio_claim_unused_sm(pio, true);
        uint32_t bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + psm);
        pio_sm_clear_fifos(pio, (uint)psm);
        pio->fdebug = bit;
        for (int i = 0; i < 4; i++) pio_sm_exec(pio, (uint)psm, pio_encode_push(false, false));
        bool before = pio->fdebug & bit;
        pio_sm_exec(pio, (uint)psm, pio_encode_push(false, false));
        bool after = pio->fdebug & bit;
        pio_sm_clear_fifos(pio, (uint)psm);
        pio->fdebug = bit;
        pio_sm_unclaim(pio, (uint)psm);
        snprintf(buf, sizeof buf, "fast drop flag: push noblock into a full RX FIFO sets FDEBUG.RXSTALL %s (set before the drop: %s)",
                 after ? "yes" : "no", before ? "yes" : "no");
        line(buf);
        if (!after || before) rc = -1;
    }
    bus_engine_check_drops();                        /* clear anything from before the test */
#ifdef PICOCO_BOARD_PLUSW
    /* The fake decode's own delay, measured, so it can be taken out. */
    int dmin, dmax, dn, lmin, lmax, ln;
    fast_at(0, 38);
    probe_arm(); fast_burst(&r); probe_done(false, &dmin, &dmax, &dn);
    fast_pattern(D_SCS); fast_set_late(4, D_SCS); fast_at(0, 38);
    probe_arm(); fast_burst(&r); probe_done(true, &lmin, &lmax, &ln);
    fast_set_late(0, 0); fast_pattern(D_CTS);
    snprintf(buf, sizeof buf, "fast fake decode delay: E rise -> OE_BUS low %d..%d clk (%d cycles); late /SCS fall -> OE_BUS low %d..%d clk (%d cycles)",
             dmin, dmax, dn, lmin, lmax, ln);
    line(buf);
    if (dn == 0) { dmin = dmax = 4; line("fast fake decode delay not captured: assuming 4"); }
    /* realistic point: 36 clk after OE_BUS fell = 36 + the fake's worst delay after E rose */
    int sreal = 36 + dmax; if (sreal > S0() + SUM(0)) sreal = S0() + SUM(0);
    snprintf(buf, sizeof buf, "fast realistic point: S=%d after E rose = %d after the fake OE_BUS fell", sreal, sreal - dmax);
    line(buf);
#define SREAL sreal
#define SREAL89 (72 + dmax)
#else
#define SREAL 36
#define SREAL89 72
#endif
    {   /* (a) realistic, 1.79 MHz: back-to-back cart reads, and core1 keeping up with them */
        uint32_t c0 = bus_stats.cycles, d0 = bus_stats.event_drop, l0 = bus_stats.event_lap;
        bus_stats.event_lag_max = 0;
        for (int i = 0; i < 5; i++) {
            fast_at(0, SREAL);
            fast_burst(&r);
            fast_line(line, "1.79MHz", SREAL, &r);
            if (r.mism || r.lost) rc = -1;
        }
        bus_engine_check_drops();
        uint32_t counted = bus_stats.cycles - c0, drop = bus_stats.event_drop - d0, lap = bus_stats.event_lap - l0;
        snprintf(buf, sizeof buf, "fast event rate 1.79MHz: counted %lu of %u, lag max %lu, drop %lu, lap %lu",
                 (unsigned long)counted, (unsigned)(5 * FAST_N), (unsigned long)bus_stats.event_lag_max, (unsigned long)drop, (unsigned long)lap);
        line(buf);
        if (counted != 5 * FAST_N || drop || lap || bus_stats.event_lag_max >= BUS_EV_LAG_CAP) rc = -1;
    }
    fast_mix();                                      /* reads with writes between them: writes must never be driven */
    fast_at(0, SREAL);
    fast_burst(&r);
    fast_line(line, "1.79MHz mixed r/w", SREAL, &r);
    if (r.mism || r.lost) rc = -1;
    fast_gaps();                                     /* unselected gaps: never driven, no stale trigger */
    fast_at(0, SREAL);
    fast_burst(&r);
    fast_line(line, "1.79MHz gaps r/w/unsel", SREAL, &r);
    if (r.mism || r.lost) rc = -1;
    {   /* writes with data, reads, I/O and gaps: write capture and event content */
        fast_wpattern();
        fast_at(0, SREAL);
        bus_trace_freeze(false);
        uint32_t c0 = bus_stats.cycles, ov0 = bus_stats.write_overrun;
        wdata_arm();
        fast_burst(&r);
        wdata_disarm();
        fast_line(line, "1.79MHz writes r/w/io/unsel", SREAL, &r);
        if (r.mism || r.lost) rc = -1;
        if (fast_check_writes(line, ov0)) rc = -1;
        if (!fast_check_events(line, c0)) rc = -1;
        fast_gaps();
        fast_at(0, SREAL);
    }
    uint32_t nsel = 0;
    for (int k = 0; k < FAST_N; k++) if (D_SEL(fast_desc[k])) nsel++;
    uint32_t ec0 = bus_stats.cycles;
    fast_restart = true;                             /* engine restarted every 50 us under the burst */
    fast_burst(&r);
    fast_restart = false;
    uint32_t evs = bus_stats.cycles - ec0;           /* the event SM (and the Plus-W helper) never stop: every cycle counts */
    snprintf(buf, sizeof buf, "fast 1.79MHz restarts %lu under a gaps burst: spurious %lu wrong %lu (zero %lu = cycles lost to a restart); events %lu of %lu",
             (unsigned long)r.restarts, (unsigned long)r.spur, (unsigned long)r.wrong, (unsigned long)r.zero, (unsigned long)evs, (unsigned long)nsel);
    line(buf);
    if (r.spur || r.wrong || r.lost || evs != nsel) rc = -1;
    if (r.spur_k >= 0) {
        snprintf(buf, sizeof buf, "fast 1.79MHz first spurious: cycle %d (%s, idx %04x) read %02x; previous cycle's byte %02x",
                 r.spur_k, (fast_desc[r.spur_k] & D_RD) ? "unselected" : "write", (unsigned)(fast_desc[r.spur_k] & 0x3FFF), r.spur_got, r.spur_prev);
        line(buf);
    }
    if (opts & FAST_OPT_RESTARTS) {                  /* repeat just the restart burst */
        uint32_t tot_rs = 0, tot_sp = 0, tot_wr = 0, tot_z = 0, bursts = 0;
        uint32_t ev_short = 0;
        for (int i = 0; i < 40; i++) {
            uint32_t e0 = bus_stats.cycles;
            fast_restart = true; fast_burst(&r); fast_restart = false;
            ev_short += nsel - (bus_stats.cycles - e0);
            tot_rs += r.restarts; tot_sp += r.spur; tot_wr += r.wrong; tot_z += r.zero; bursts++;
            if (r.spur_k >= 0) {
                snprintf(buf, sizeof buf, "fast restarts: burst %d spurious %lu, first at cycle %d (%s) read %02x, previous cycle's byte %02x",
                         i, (unsigned long)r.spur, r.spur_k, (fast_desc[r.spur_k] & D_RD) ? "unselected" : "write", r.spur_got, r.spur_prev);
                line(buf);
            }
        }
        snprintf(buf, sizeof buf, "fast restarts: %lu bursts, %lu restarts, spurious %lu wrong %lu zero %lu, events missing %ld",
                 (unsigned long)bursts, (unsigned long)tot_rs, (unsigned long)tot_sp, (unsigned long)tot_wr, (unsigned long)tot_z, (long)(int32_t)ev_short);
        line(buf);
        if (tot_sp || tot_wr || ev_short) rc = -1;
        goto done;
    }
    fast_burst(&r);
    fast_line(line, "1.79MHz gaps after restarts", SREAL, &r);
    if (r.mism || r.lost) rc = -1;
    {   /* lap: core1 held off the ring (it parks at its next 256-event check)
         * through a gaps burst of 3072 events, so DMA C laps it; let go ~200 us
         * into the reads burst after it, so the resync meets a running DMA C;
         * then the next reads burst in order (the trace check), every event
         * counted */
        uint32_t l0 = bus_stats.event_lap;
        bus_core1_hold = true;
        fast_burst(&r);                              /* r.lost: the ring never drains while core1 is parked */
        fast_pattern(D_CTS);
        fast_at(0, SREAL);
        fast_mid_burst = release_hold;
        fast_burst(&r);
        fast_mid_burst = NULL;
        bool drained = events_drained(), relbad = r.mism || r.lost;
        uint32_t laps = bus_stats.event_lap - l0, e0 = bus_stats.cycles;
        fast_burst(&r);
        uint32_t evs = bus_stats.cycles - e0;
        bool inorder = fast_check_events(line, e0);
        snprintf(buf, sizeof buf, "fast lap: resync %lu, next burst events %lu of %u", (unsigned long)laps, (unsigned long)evs, FAST_N);
        line(buf);
        if (!laps || !drained || relbad || !inorder || evs != FAST_N || r.mism || r.lost) rc = -1;
    }
    {   /* stall: DMA A paused for a reads burst, so the read SM waits at `pull`
         * from the first cycle on; then left stalled (bus_engine_test_stall) for
         * the guard, run here on made-up ms. The next burst runs with the guard
         * ticking on every pass: a busy bus must never look stalled. */
        uint32_t s0 = bus_stats.engine_stall;
        fast_pattern(D_CTS);
        fast_at(0, SREAL);
        bus_engine_test_stall(true);
        fast_burst(&r);
        bus_engine_test_stall(false);
        uint32_t driven = (FAST_N - r.zero) + r.rel_bad;   /* reads not 0x00, ends not released */
        for (uint32_t t = 1; t <= 3; t++) bus_engine_tick(t);
        fast_tick = 4;
        fast_burst(&r);
        fast_tick = 0;
        uint32_t det = bus_stats.engine_stall - s0;
        char drv[24] = "never driven";
        if (driven) snprintf(drv, sizeof drv, "driven %lu times", (unsigned long)driven);
        snprintf(buf, sizeof buf, "fast stall: detected %lu, pins %s during the stall, next burst %lu mismatches",
                 (unsigned long)det, drv, (unsigned long)r.mism);
        line(buf);
        if (det != 1 || driven || r.mism || r.lost) rc = -1;
        fast_gaps();
        fast_at(0, SREAL);
    }
    fast_fill_banks();                               /* banks 0 and 1 differ for the switch bursts */
    bus_engine_set_bank(0);
    if (opts & FAST_OPT_SWITCHES) {                  /* 40 switch bursts, reads and gaps in turn */
        sw_res_t t;
        memset(&t, 0, sizeof t);
        uint32_t badb = 0;
        for (int i = 0; i < 40; i++) {
            if (i & 1) fast_gaps(); else fast_pattern(D_CTS);
            fast_at(0, SREAL);
            fast_switch = true; fast_burst(&r); fast_switch = false;
            bus_engine_set_bank(0);
            t.switches += swr.switches; t.bad += swr.bad; t.wrong_bank += swr.wrong_bank; t.next_old += swr.next_old;
            for (int p = 0; p <= 84 / SW_BIN; p++) { t.ph_old[p] += swr.ph_old[p]; t.ph_new[p] += swr.ph_new[p]; }
            if (swr.bad || swr.wrong_bank || r.lost) {
                badb++;
                snprintf(buf, sizeof buf, "fast switches: burst %d (%s) bad %lu wrong bank %lu lost %lu, first bad cycle %d got %02x want %02x, pio0 irq %02lx",
                         i, (i & 1) ? "gaps" : "reads", (unsigned long)swr.bad, (unsigned long)swr.wrong_bank, (unsigned long)r.lost,
                         swr.bad_k, swr.bad_got, swr.bad_exp, (unsigned long)pio0->irq);
                line(buf);
                bus_engine_drive(true);
            }
        }
        snprintf(buf, sizeof buf, "fast switches: %lu in 40 bursts (S=%d), %lu bursts bad; bad %lu, wrong bank %lu; next cycle old %lu",
                 (unsigned long)t.switches, SREAL, (unsigned long)badb, (unsigned long)t.bad, (unsigned long)t.wrong_bank, (unsigned long)t.next_old);
        line(buf);
        for (int h = 0; h < 2; h++) {                /* the cycle the exec landed in, by phase: new/old */
            int pos = snprintf(buf, sizeof buf, "fast switches phase %s (clk:new/old)", h ? "42-83" : "0-41");
            for (int p = h * (42 / SW_BIN + 1); p <= (h ? 84 / SW_BIN : 42 / SW_BIN); p++)
                pos += snprintf(buf + pos, sizeof buf - pos, " %d:%lu/%lu", p * SW_BIN, (unsigned long)t.ph_new[p], (unsigned long)t.ph_old[p]);
            line(buf);
        }
        if (badb) rc = -1;
        goto done;
    }
    for (int g = 0; g < 2; g++) {                    /* bank switches every 50 us at a random phase: reads, then gaps */
        if (g) fast_gaps(); else fast_pattern(D_CTS);
        fast_at(0, SREAL);
        fast_switch = true;
        fast_burst(&r);
        fast_switch = false;
        bus_engine_set_bank(0);
        uint32_t old = 0;
        for (int p = 0; p <= 84 / SW_BIN; p++) old += swr.ph_old[p];
        snprintf(buf, sizeof buf, "fast 1.79MHz bank switches %lu under a %s burst S=%d: bad %lu, wrong bank %lu, lost %lu; old-bank reads %lu, next cycle old %lu",
                 (unsigned long)swr.switches, g ? "gaps" : "reads", SREAL, (unsigned long)swr.bad, (unsigned long)swr.wrong_bank,
                 (unsigned long)r.lost, (unsigned long)old, (unsigned long)swr.next_old);
        line(buf);
        if (swr.bad || swr.wrong_bank || r.lost) rc = -1;
    }
    {   /* capture-only: the read SM stopped, the event SM and DMA C running:
         * nothing driven, every cycle an event, every write captured */
        fast_wpattern();
        fast_at(0, SREAL);
        uint32_t n = 0, c0 = bus_stats.cycles, ov0 = bus_stats.write_overrun;
        for (int k = 0; k < FAST_N; k++) if (D_SEL(fast_desc[k])) n++;
        bus_drive_set(false);
        wdata_arm();
        fast_burst(&r);
        wdata_disarm();
        bus_drive_set(true);
        uint32_t evs = bus_stats.cycles - c0, wbad = fast_check_writes(NULL, ov0);
        snprintf(buf, sizeof buf, "fast drive off: %lu driven, events %lu of %lu, writes %lu lost",
                 (unsigned long)r.spur, (unsigned long)evs, (unsigned long)n, (unsigned long)wbad);
        line(buf);
        if (r.mism || r.lost || evs != n || wbad) rc = -1;
    }
    {   /* banks: the bank number is part of the read SM's pointer */
        fast_fill_banks();
        int good = 0;
        uint32_t bmis = 0;
        for (int b = 0; b < BUS_BANKS; b++) {
            bus_engine_set_bank((uint8_t)b);
            fast_pattern(D_CTS);
            fast_add_io();
            fast_at(0, SREAL);
            fast_burst(&r);
            if (r.mism || r.lost) {
                snprintf(buf, sizeof buf, "fast bank %d", b);
                fast_line(line, buf, SREAL, &r);
            } else good++;
            bmis += r.mism;
        }
        bus_engine_set_bank(0);
        snprintf(buf, sizeof buf, "fast banks: %d/%d banks read their own pattern, %lu mismatches", good, BUS_BANKS, (unsigned long)bmis);
        line(buf);
        if (good != BUS_BANKS) rc = -1;
        int same = 0;
        for (int b = 0; b < BUS_BANKS; b++) same += memcmp(&bus_mem[b][BUS_IO_LO], &bus_mem[0][BUS_IO_LO], BUS_IO_HI - BUS_IO_LO + 1) == 0;
        snprintf(buf, sizeof buf, "fast io page: same in %d banks", same);
        line(buf);
        if (same != BUS_BANKS) rc = -1;
    }
    {   /* Becker at speed: the client loop at both speeds (the 1.79 MHz one
         * also the event rate with the hooks running), then status and data
         * on consecutive cycles */
        bk_res_t b;
        bk_setup();
        becker_refresh();                            /* fast_fill's pattern is in $FF41/$FF42; no cycle runs, so core1 is in no hook */
        for (int sp = 0; sp < 2; sp++) {
            fast_at(sp ? 3 : 0, sp ? SREAL89 : SREAL);
            uint32_t un0 = becker_stats.underrun, ov0 = becker_stats.overrun;
            uint32_t c0 = bus_stats.cycles, d0 = bus_stats.event_drop, l0 = bus_stats.event_lap;
            bus_stats.event_lag_max = 0;
            bk_run(NULL, BK_N, &b);
            bool drained = events_drained();
            bus_engine_check_drops();
            if (!bk_stream_line(line, sp ? "0.89MHz" : "1.79MHz", &b, un0, ov0) || !drained) rc = -1;
            if (sp) continue;
            uint32_t counted = bus_stats.cycles - c0, drop = bus_stats.event_drop - d0, lap = bus_stats.event_lap - l0;
            snprintf(buf, sizeof buf, "fast event rate with Becker 1.79MHz: counted %lu of %lu, lag max %lu, drop %lu, lap %lu",
                     (unsigned long)counted, (unsigned long)b.cycles, (unsigned long)bus_stats.event_lag_max, (unsigned long)drop, (unsigned long)lap);
            line(buf);
            if (counted != b.cycles || drop || lap || bus_stats.event_lag_max >= BUS_EV_LAG_CAP) rc = -1;
        }
        /* a byte published by an earlier poll, then the port empty (that data
         * read is an underrun, by design) */
        static const uint8_t b2b[] = { 0, 1, 2, 3, BK_S, 0, 1, 2, 3, BK_S, BK_D, 0, 1, 2, 3 };
        uint8_t res[5][4];
        static const uint8_t want[4] = { 0x02, 0x5C, 0x00, 0xFF };
        bool stable = true, drained = true;
        fast_at(0, SREAL);
        for (int i = 0; i < 5; i++) {
            uint8_t x = 0x5C;
            becker_write(&x, 1);
            bk_run(b2b, sizeof b2b, &b); res[i][0] = b.last_st; res[i][1] = b.last_d;
            drained &= events_drained();
            bk_run(b2b, sizeof b2b, &b); res[i][2] = b.last_st; res[i][3] = b.last_d;
            drained &= events_drained();
            if (memcmp(res[i], res[0], 4)) stable = false;
        }
        bus_trace_freeze(false);                     /* the empty-port reads froze it */
        snprintf(buf, sizeof buf, "fast becker back-to-back: ready -> %02x,%02x; empty -> %02x,%02x (%s)%s",
                 res[0][0], res[0][1], res[0][2], res[0][3], stable ? "stable" : "NOT stable", drained ? "" : ", events NOT drained");
        line(buf);
        if (!stable || !drained || memcmp(res[0], want, 4)) rc = -1;
        /* information: a poll on the cycle right after the read that took the
         * last byte, before that read's hook has popped it (a 6809 client
         * has a few cycles between the two) */
        static const uint8_t d2s[] = { 0, 1, 2, 3, BK_S, 0, 1, 2, 3, BK_D, BK_S, 0, 1, 2, 3 };
        uint8_t after[2];
        for (int sp = 0; sp < 2; sp++) {
            uint8_t x = 0x5C;
            fast_at(sp ? 3 : 0, sp ? SREAL89 : SREAL);
            becker_write(&x, 1);
            bk_run(d2s, sizeof d2s, &b);
            after[sp] = b.last_st;
            events_drained();
        }
        snprintf(buf, sizeof buf, "fast becker data then status: the poll right after the last byte's read sees %02x at 1.79MHz, %02x at 0.89MHz (00 = not ready)",
                 after[0], after[1]);
        line(buf);
    }
    {   /* $FF40 at speed, through rom.c's hook, over the banks filled above */
        rom_publish_banks(BUS_BANKS);
        fast_bank_pattern();
        for (int sp = 1; sp >= 0; sp--) {
            bus_engine_set_bank(0);
            fast_at(sp ? 3 : 0, sp ? SREAL89 : SREAL);
            wdata_arm();
            fast_mid_burst = fast_hook_latency;
            fast_burst(&r);
            fast_mid_burst = NULL;
            wdata_disarm();
            uint32_t h[4], bad;
            fast_bank_score(h, &bad);
            int k = h[3] ? 3 : h[2] ? 2 : h[1] ? 1 : 0;   /* the latest any switch took */
            snprintf(buf, sizeof buf, "fast bank switch %s: new bank at +%d cycles%s (hook %lu..%lu clk); %u switches: +0 %lu, +1 %lu, +2 %lu, later %lu; bad %lu, lost %lu",
                     sp ? "0.89MHz" : "1.79MHz", k, k == 3 ? " (not by +2)" : "", (unsigned long)hl_min, (unsigned long)hl_max, FAST_N / 8,
                     (unsigned long)h[0], (unsigned long)h[1], (unsigned long)h[2], (unsigned long)h[3], (unsigned long)bad, (unsigned long)r.lost);
            line(buf);
            /* +0 or +1 at 0.89 MHz (spec 5: a switch may fetch one byte from
             * the old bank), 1.79 MHz reported only. hook: OE_BUS rise of the
             * write to the hook's bus_bank store, over 16 writes. */
            if (bad || r.lost || !hl_n || (sp && k > 1)) rc = -1;
        }
        rom_banks_begin();                           /* unbanked: the hook is inert again */
    }
    fast_pattern(D_CTS);
#ifdef PICOCO_BOARD_PLUSW
    /* late select: /CTS (ROM reads) and /SCS (I/O reads) fall 4 clk after E rose */
    for (int which = 0; which < 2; which++) {
        uint32_t sel = which ? D_SCS : D_CTS;
        const char *tag = which ? "1.79MHz late /SCS" : "1.79MHz late /CTS";
        fast_pattern(sel);
        fast_set_late(4, sel);
        fast_at(0, SREAL);
        fast_burst(&r);
        fast_line(line, tag, SREAL, &r);
        if (r.mism || r.lost) rc = -1;
        fast_mix();
        fast_at(0, SREAL);
        fast_burst(&r);
        snprintf(buf, sizeof buf, "%s + writes", tag);
        fast_line(line, buf, SREAL, &r);
        if (r.mism || r.lost) rc = -1;
        fast_pattern(sel);
        int lr = fast_sweep(line, tag, 0);
        int dl = lmax;                              /* both late selects take the same poll path; the /SCS probe stands for both */
        snprintf(buf, sizeof buf, "fast %s: select fell at %d; response %d clk after the select fell, %d after the fake OE_BUS fell (fake delay %d)",
                 tag, fast_L + 1, lr - (fast_L + 1), lr - (fast_L + 1) - dl, dl);
        line(buf);
        if (lr < 0) rc = -1;
    }
    fast_set_late(0, 0);
    fast_pattern(D_CTS);
#endif
    int resp = fast_sweep(line, "1.79MHz", 0);       /* (b) */
#ifdef PICOCO_BOARD_PLUSW
    snprintf(buf, sizeof buf, "fast 1.79MHz response_clk corrected %d..%d after the fake OE_BUS fell (raw %d minus the fake delay %d..%d); margin to 36: %d..%d",
             resp - dmax, resp - dmin, resp, dmin, dmax, 36 - (resp - dmin), 36 - (resp - dmax));
    line(buf);
    if (resp < 0 || resp - dmax > 36) rc = -1;
#else
    if (resp < 0 || resp > 36) rc = -1;
#endif
    {   /* release: earliest point after the end of the cycle where D0-D7 read 0 for the whole burst */
        int rel_ok = -1;
        fast_at(0, SREAL);
        for (int k = 0; k <= 13 && rel_ok < 0; k++) { fast_set_rel(k); fast_burst(&r); if (!r.rel_bad && !r.mism) rel_ok = k + 1; }
        fast_set_rel(3);
#ifdef PICOCO_BOARD_PLUSW
        snprintf(buf, sizeof buf, "fast 1.79MHz release_clk %d (D0-D7 low on every cycle %d clk after E fell; -1 = not by 14)", rel_ok, rel_ok);
#else
        snprintf(buf, sizeof buf, "fast 1.79MHz release_clk %d (D0-D7 low on every cycle %d clk after OE_BUS rose; -1 = not by 14)", rel_ok, rel_ok);
#endif
        line(buf);
    }
    fast_at(3, SREAL89);                              /* (c) 0.89 MHz: high 84, low 84 */
    fast_burst(&r);
    fast_line(line, "0.89MHz", SREAL89, &r);
    if (r.mism || r.lost) rc = -1;
    fast_sweep(line, "0.89MHz", 3);
done:
#undef SREAL
#undef SREAL89

    pio_sm_set_enabled(pio, sm, false);
#ifdef PICOCO_BOARD_PLUSW
    decode_stop();
#endif
    dma_channel_unclaim(fdma_tx);
    dma_channel_unclaim(fdma_rx);
    pio->input_sync_bypass = bypass_was;
    pins_to_sio();
    pio_remove_program(pio, &FAST_PROG, offset);
    pio_sm_unclaim(pio, sm);
    sm = -1;
    for (int i = 0; i < 32; i++) bus_io_set((uint16_t)(FAST_IO + i), io_save[i]);
    rom_off();
    bus_drive_set(drive_was);
    line("selftest rom cleared; reload with rom load");
    return rc;
}
#endif
