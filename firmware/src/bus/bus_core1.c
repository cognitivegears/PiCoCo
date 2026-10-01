#include "bus.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include PICOCO_BOARD_H

/* OE_BUS lives in a different SIO register on each board (gpio_in on a Pico
 * 2, gpio_hi_in on a Plus-W where it's GP40) — BUS_OE_REG/BUS_OE_MASK come
 * from the board header so this file reads the right one either way.
 * Address/data/RW are always in gpio_in on both boards. */
#define OE_HIGH() (sio_hw->BUS_OE_REG & BUS_OE_MASK)
#define RW_MASK (1u << PIN_RW)
#define D_MASK  (0xFFu << PIN_D0)

/* core1 entry: launched once from main.c after config replay, never returns.
 * Flash-free per Plan B - everything reachable from here must be BUS_HOT or
 * static inline (see the nm/objdump acceptance check in the task report). */
#ifndef PICOCO_BOARD_PLUSW
/* Pico 2: OE_BUS (GP26) is in gpio_in, so one sample carries OE, address, R/W. */
#define OE_LOW_IN(x) (!((x) & BUS_OE_MASK))

#define KEY_MASK ((0x3FFFu << PIN_A0) | RW_MASK)

/* Precompute while idle, enable on the edge. At 1.79 MHz (CoCo 3 fast mode,
 * all of NitrOS-9) E is high for 279 ns and the CPU latches data at E fall.
 * Address and R/W are valid from Q, a quarter cycle before E, so while OE_BUS
 * is high the loop keeps the response for the address on the bus sitting in
 * the output latch (outputs disabled), and the only work after OE_BUS falls is
 * one compare and one output-enable store.
 *
 * The idle loop is deliberately tiny (~8 clk_sys cycles a pass): it recomputes
 * only when address or R/W change, and a recompute pass is ~33 cycles, so keep
 * work out of both. Static counts, not scope measurements: a hit enables
 * ~55-105 ns after OE_BUS falls; OE_BUS falling during a recompute pass or a
 * miss can be 200 ns or more. The 2026-09-30 version recomputed on every pass
 * and then did three stores. Bench 2026-10-01
 * (NitrOS-9 over the bridge): 0.7-4 % of sectors had a byte read as 0x00, the
 * rate moving with unrelated code changes, i.e. the tail of that spread was
 * past the latch point.
 *
 * A cycle whose OE-low sample does not match the precomputed key (address
 * still settling, or back-to-back cycles with no idle sample) is served from
 * the fresh sample and counted, as before. */
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();          /* never restored: core1 does nothing else */
    uint32_t in;
    for (;;) {
        uint32_t key = ~0u;                       /* no idle sample yet: cannot match a real sample */
        uint32_t oemask = 0;
        for (;;) {                                /* idle: keep the latch loaded for the address on the bus */
            in = sio_hw->gpio_in;
            if (OE_LOW_IN(in)) break;             /* `in` is the sample that saw the cycle start */
            uint32_t k = in & KEY_MASK;
            if (k != key) {
                /* Address lines do not all arrive together (A7 lags a few ns).
                 * A recompute pass is long, so do not spend one on a sample that
                 * is still moving: look again first. */
                if ((sio_hw->gpio_in ^ in) & (KEY_MASK | BUS_OE_MASK)) continue;
                key = k;
                oemask = ((in & RW_MASK) && bus_drive) ? D_MASK : 0;   /* never enable into a write */
                if (oemask) {                     /* latch only (outputs are off), and only for a read we will
                                                   * drive: `bus selftest` puts its write data in this latch */
                    sio_hw->gpio_clr = D_MASK;
                    sio_hw->gpio_set = (uint32_t)bus_peek((in >> PIN_A0) & 0x3FFF) << PIN_D0;
                }
            }
        }
        bool hit = (in & KEY_MASK) == key;
        if (hit) sio_hw->gpio_oe_set = oemask;
        bool rd = (in & RW_MASK) != 0;
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        if (rd) {                                 /* CoCo read */
            if (bus_drive) {
                if (!hit) {                       /* precompute was for another address, or there was none: drive first, count after */
                    sio_hw->gpio_clr = D_MASK;
                    sio_hw->gpio_set = (uint32_t)bus_peek(idx) << PIN_D0;
                    sio_hw->gpio_oe_set = D_MASK;
                    /* addr_resample here is mostly address skew: a sample taken while the
                     * lines were still changing (bench 2026-10-01: A7 a few ns behind
                     * the rest, ~5 % of reads) followed by no clean idle sample before
                     * OE_BUS fell. Served from the OE-low sample, ~165 ns later than a hit. */
                    if (key != ~0u) { bus_stats.resample_key = key; bus_stats.resample_in = in; bus_stats.addr_resample++; bus_stats.addr_resample_bits |= (idx ^ ((key >> PIN_A0) & 0x3FFF)); }
                    else bus_stats.late_precompute++;   /* back-to-back cycles (boot ROM copy): expected */
                }
                /* End of cycle = OE_BUS high on three samples running, so a
                 * spike on OE_BUS cannot clear the pads mid-cycle. Cheap guard:
                 * oe_glitch has read 0 on the bench so far. */
                for (;;) {
                    if (!OE_HIGH()) continue;
                    if (OE_HIGH() && OE_HIGH()) break;
                    bus_stats.oe_glitch++;
                }
                sio_hw->gpio_clr = D_MASK;        /* leave the pads at 0 V, see DISCHARGE below */
                sio_hw->gpio_clr = D_MASK;        /* second store: two clk_sys cycles of drive, as on the write path */
                sio_hw->gpio_oe_clr = D_MASK;
            } else {
                while (!OE_HIGH()) { }
                sio_hw->gpio_oe_clr = D_MASK;     /* bus_drive went false after the precompute enabled us */
            }
            bus_on_read_done(idx, time_us_32());
        } else {                                  /* CoCo write: use the last gpio_in sample taken
                                                    * while OE_BUS was still low, not the first one
                                                    * with OE_BUS high (U10 may have begun
                                                    * tri-stating by then). prev starts at `in`,
                                                    * itself sampled while OE_BUS was low above. */
            uint32_t d, prev = in;
            for (;;) {
                d = sio_hw->gpio_in;
                if (OE_HIGH()) break;
                prev = d;
            }
            /* DISCHARGE: RP2350-E9 - a pad left above ~1 V with only the internal
             * pull-down sits near 2.2 V, so the bits the CoCo just wrote would
             * still read high through U10 at the start of the next read cycle.
             * A late $FF41 poll then showed bit 1 from the written byte, the
             * Becker "ready" bit (bench 2026-10-01, NitrOS-9 boot: phantom byte
             * after a $26 write). Drive low for two SIO writes, then release;
             * OE_BUS is already high, so U10 is off. */
            sio_hw->gpio_clr = D_MASK;
            sio_hw->gpio_oe_set = D_MASK;
            sio_hw->gpio_clr = D_MASK;
            sio_hw->gpio_oe_clr = D_MASK;
            bus_on_write(idx, (uint8_t)((prev >> PIN_D0) & 0xFF), time_us_32());
        }
    }
}
#else  /* PICOCO_BOARD_PLUSW */
#define E_MASK     (1u << PIN_E)
#define Q_MASK     (1u << PIN_Q)
#define CTS_MASK   (1u << PIN_CTS)
#define SCS_MASK   (1u << PIN_SCS)
#define A14_MASK   (1u << PIN_A14)
#define A15_MASK   (1u << PIN_A15)
#define OEFW_MASK  (1u << PIN_OE_FW)
#define DECODE_MASK ((0x3FFFu << PIN_A0) | RW_MASK | CTS_MASK | SCS_MASK | A14_MASK | A15_MASK)

/* Selected = /CTS or /SCS asserted, or the full 16-bit address is in bus_fw_mask. */
static inline __attribute__((always_inline)) bool plusw_selected(uint32_t in) {
    if ((in & (CTS_MASK | SCS_MASK)) != (CTS_MASK | SCS_MASK)) return true;
    uint16_t addr = ((in >> PIN_A0) & 0x3FFF) | ((in & A14_MASK) ? 0x4000 : 0) | ((in & A15_MASK) ? 0x8000 : 0);
    return bus_fw_selected(addr, bus_fw_mask);
}

/* Plus-W: decode during E low, act during E high. Works with JP2 in either
 * position (1-2: U15 also enables U10 for hardware-selected cycles, which is
 * consistent with what we do; 2-3: only PIN_OE_FW enables it, which is what
 * lets $FF60-$FF7F respond). Reads take a fast path: the response (index,
 * data byte, hw/fw stat bucket) is precomputed from the Q-time sample while
 * E is still low, so the work after E rises is one compare and four pin
 * writes. Any decode-bit change caught at the E-rise resample falls through
 * to the full path below, which recomputes everything from the fresh
 * sample. */
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    /* E-low wait lives here, once, not at the top of the loop: post-cycle
     * work (a write hook, bus_on_read_done) that overruns E low must not
     * drop the next real cycle. The Pico 2 loop already serves a late
     * cycle instead of skipping it (its top just waits for OE_BUS low,
     * which is still true if E stayed high the whole time core1 was
     * busy); this loop matches that by starting each iteration at the
     * Q-high wait instead of re-checking E low first. */
    while (sio_hw->gpio_in & E_MASK) { }
    for (;;) {
        while (!(sio_hw->gpio_in & Q_MASK)) { }         /* wait Q high: address valid */
        uint32_t in = sio_hw->gpio_in;
        bool sel = plusw_selected(in);                  /* head start from the Q-time sample */
        /* Precompute the read response during E low: after E rises the fast path
         * below is one compare and four pin writes. Any change in the decode
         * bits at E rise (late /CTS, address settling) falls through to the
         * full path, which recomputes everything from the fresh sample. */
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        uint32_t dset = (uint32_t)bus_peek(idx) << PIN_D0;
        bool fast_read = sel && (in & RW_MASK) && bus_drive;
        while (!(sio_hw->gpio_in & E_MASK)) { }         /* wait E high */
        /* /CTS and /SCS are decoded downstream of the address (SAM/GIME) and need only
         * meet setup before E rises, so re-check them now; the Pico 2 bench needed the
         * same resample. */
        uint32_t in2 = sio_hw->gpio_in;
        if (fast_read && !((in ^ in2) & DECODE_MASK)) {
            sio_hw->gpio_clr = D_MASK;
            sio_hw->gpio_set = dset;
            sio_hw->gpio_oe_set = D_MASK;
            sio_hw->gpio_clr = OEFW_MASK;               /* U10 outward */
            while (sio_hw->gpio_in & E_MASK) { }
            sio_hw->gpio_set = OEFW_MASK;
            sio_hw->gpio_oe_clr = D_MASK;
            bus_on_read_done(idx, time_us_32());
            if ((in & (CTS_MASK | SCS_MASK)) != (CTS_MASK | SCS_MASK)) bus_stats.hw_selected++; else bus_stats.fw_selected++;
            continue;
        }
        uint32_t dif = (in ^ in2) & DECODE_MASK;
        if (dif) {
            bus_stats.addr_resample++;
            bus_stats.addr_resample_bits |= (dif >> PIN_A0) & 0x3FFF;
            in = in2;
            sel = plusw_selected(in);
        }
        bool hw = ((in & (CTS_MASK | SCS_MASK)) != (CTS_MASK | SCS_MASK));
        /* OE_BUS is U15's own E-qualified hardware decode (readable on GP40
         * in either JP2 position); catches a /CTS or /SCS that asserts too
         * late for the resample above to see. A cycle rescued this way is
         * hardware-selected by definition, even though `in`'s /CTS,/SCS
         * bits read high. */
        if (!sel) { sel = !OE_HIGH(); if (sel) hw = true; }
        if (!sel) { while (sio_hw->gpio_in & E_MASK) { } continue; }   /* not selected: still wait out E low before the next cycle */
        idx = (in >> PIN_A0) & 0x3FFF;                  /* declared above; `in` may now be in2 */
        if (in & RW_MASK) {                             /* CoCo read */
            if (bus_drive) {
                sio_hw->gpio_clr = D_MASK;
                sio_hw->gpio_set = (uint32_t)bus_peek(idx) << PIN_D0;
                sio_hw->gpio_oe_set = D_MASK;
                sio_hw->gpio_clr = OEFW_MASK;           /* U10 outward */
                while (sio_hw->gpio_in & E_MASK) { }
                sio_hw->gpio_set = OEFW_MASK;
                sio_hw->gpio_oe_clr = D_MASK;
            } else {
                while (sio_hw->gpio_in & E_MASK) { }
            }
            bus_on_read_done(idx, time_us_32());
        } else {                                        /* CoCo write: last sample while E was high */
            /* Also in capture-only mode: U10's direction is RW_BUF (hardware), so enabling
             * it here only lets the CoCo's write data in. */
            sio_hw->gpio_clr = OEFW_MASK;               /* U10 inward */
            uint32_t d, prev = sio_hw->gpio_in;
            for (;;) {
                d = sio_hw->gpio_in;
                if (!(d & E_MASK)) break;
                prev = d;
            }
            sio_hw->gpio_set = OEFW_MASK;
            bus_on_write(idx, (uint8_t)((prev >> PIN_D0) & 0xFF), time_us_32());
        }
        /* Counted after servicing, not in the E-rise-to-data window: keeps
         * that window free of anything but the transfer itself. */
        if (hw) bus_stats.hw_selected++; else bus_stats.fw_selected++;
    }
}
#endif
