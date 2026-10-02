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
/* Pico 2: the PIO + DMA engine (bus_engine.pio) serves ROM-window and I/O-page
 * reads with no CPU in the per-cycle path; core1 has nothing to do. The CPU
 * loop this replaced is at tag fw-1.4-cpu-loop. Spike scope: no read hooks,
 * no write capture, no trace or counters, so the Becker port does not work on
 * this build.
 *
 * DMA A: PIO0 RX (the table byte's address) -> DMA B READ_ADDR_TRIG, endless.
 * DMA B: that byte -> PIO0 TX, 8-bit, count 1, re-armed by every A write.
 * Everything below runs on core0. */
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/structs/busctrl.h"
#include "bus_engine.pio.h"

static PIO const epio = pio0;
static int esm = -1, edma_a, edma_b;
static uint eoff;
static bool erunning;
static bool ebypass = true, eprio = true;   /* measured defaults, see docs/superpowers/specs/2026-10-02-pio-engine-spike.md */

#define ENG_IN_MASK ((0x3FFFu << PIN_A0) | (1u << PIN_RW) | (1u << PIN_OE_BUS))

static void engine_init(void) {
    esm = (int)pio_claim_unused_sm(epio, true);
    eoff = pio_add_program(epio, &bus_engine_program);
    pio_sm_config c = bus_engine_program_get_default_config(eoff);
    sm_config_set_in_pins(&c, PIN_A0);
    sm_config_set_out_pins(&c, PIN_D0, 8);
    sm_config_set_jmp_pin(&c, PIN_RW);
    sm_config_set_in_shift(&c, false, false, 32);    /* shift left: isr = x << 14 | A13..A0 */
    sm_config_set_out_shift(&c, true, false, 32);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_set_pins_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);
    pio_sm_set_pindirs_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);
    for (int g = PIN_D0; g < PIN_D0 + 8; g++) pio_gpio_init(epio, (uint)g);
    pio_sm_init(epio, (uint)esm, eoff, &c);

    edma_a = dma_claim_unused_channel(true);
    edma_b = dma_claim_unused_channel(true);
    dma_channel_config b = dma_channel_get_default_config((uint)edma_b);
    channel_config_set_transfer_data_size(&b, DMA_SIZE_8);
    channel_config_set_read_increment(&b, false);
    channel_config_set_write_increment(&b, false);
    channel_config_set_dreq(&b, pio_get_dreq(epio, (uint)esm, true));
    channel_config_set_high_priority(&b, true);
    dma_channel_configure((uint)edma_b, &b, &epio->txf[esm], bus_table, 1, false);
    dma_channel_config a = dma_channel_get_default_config((uint)edma_a);
    channel_config_set_transfer_data_size(&a, DMA_SIZE_32);
    channel_config_set_read_increment(&a, false);
    channel_config_set_write_increment(&a, false);
    channel_config_set_dreq(&a, pio_get_dreq(epio, (uint)esm, false));
    channel_config_set_high_priority(&a, true);
    dma_channel_configure((uint)edma_a, &a, &dma_hw->ch[edma_b].al3_read_addr_trig, &epio->rxf[esm],
                          dma_encode_endless_transfer_count(), true);
}

static void engine_stop(void) {
    pio_sm_set_enabled(epio, (uint)esm, false);
    pio_sm_set_pins_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);      /* E9: low first... */
    pio_sm_set_pindirs_with_mask(epio, (uint)esm, 0, 0xFFu << PIN_D0);   /* ...then never left driving */
    erunning = false;
}

static void engine_start(void) {
    while (dma_channel_is_busy((uint)edma_b)) { }   /* a byte in flight lands before the FIFOs are cleared */
    pio_sm_clear_fifos(epio, (uint)esm);
    pio_sm_restart(epio, (uint)esm);
    pio_sm_put(epio, (uint)esm, (uint32_t)(uintptr_t)bus_rom_base >> 14);
    pio_sm_exec(epio, (uint)esm, pio_encode_pull(false, true));
    pio_sm_exec(epio, (uint)esm, pio_encode_mov(pio_x, pio_osr));
    pio_sm_exec(epio, (uint)esm, pio_encode_jmp(eoff + bus_engine_wrap_target));
    epio->input_sync_bypass = ebypass ? (epio->input_sync_bypass | ENG_IN_MASK) : (epio->input_sync_bypass & ~ENG_IN_MASK);
    busctrl_hw->priority = eprio ? (BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS) : 0;
    pio_sm_set_enabled(epio, (uint)esm, true);
    erunning = true;
}

void bus_engine_drive(bool on) {
    if (esm < 0) engine_init();
    if (erunning) engine_stop();
    if (on) engine_start();
}

/* core0, after bus_rom_base changed: x must point at the new window. The SM is
 * stopped for a few us; a cycle in that gap reads 0x00. */
void bus_engine_rebase(void) {
    if (erunning) { engine_stop(); engine_start(); }
}

void bus_engine_tune(int bypass, int prio) {
    if (bypass >= 0) ebypass = bypass;
    if (prio >= 0) eprio = prio;
    if (erunning) { engine_stop(); engine_start(); }
}
void bus_engine_get(bool *bypass, bool *prio) { *bypass = ebypass; *prio = eprio; }

BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    for (;;) __wfe();
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

/* End of a cycle we drove: U10 off, then the pads driven low before release
 * so no bits linger on them (RP2350-E9, see the Pico 2 loop; an A4 chip does
 * not need it, an A2 does). Everything here delays the next back-to-back
 * cycle, so there is no multi-sample E filter as on the Pico 2: bare Plus-W
 * self-test 2026-10-01, burst response 352-366 ns before this macro and
 * 425-454 ns with a three-sample filter and two clears, against 480. */
#define PLUSW_READ_END() do { \
        while (sio_hw->gpio_in & E_MASK) { } \
        sio_hw->gpio_set = OEFW_MASK; \
        sio_hw->gpio_clr = D_MASK; \
        sio_hw->gpio_oe_clr = D_MASK; \
    } while (0)

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
 * E is still low, so the work after E rises is one compare and two pin
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
         * below is one compare and two pin writes. Any change in the decode
         * bits at E rise (late /CTS, address settling) falls through to the
         * full path, which recomputes everything from the fresh sample. */
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        uint32_t dset = (uint32_t)bus_peek(idx) << PIN_D0;
        bool fast_read = sel && (in & RW_MASK) && bus_drive;
        if (fast_read) {                                /* latch only: the outputs are off until E rises */
            sio_hw->gpio_clr = D_MASK;
            sio_hw->gpio_set = dset;
        }
        while (!(sio_hw->gpio_in & E_MASK)) { }         /* wait E high */
        /* /CTS and /SCS are decoded downstream of the address (SAM/GIME) and need only
         * meet setup before E rises, so re-check them now; the Pico 2 bench needed the
         * same resample. */
        uint32_t in2 = sio_hw->gpio_in;
        if (fast_read && !((in ^ in2) & DECODE_MASK)) {
            sio_hw->gpio_oe_set = D_MASK;
            sio_hw->gpio_clr = OEFW_MASK;               /* U10 outward */
            PLUSW_READ_END();
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
                PLUSW_READ_END();
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
            /* Discharge the pads U10 was driving (see the Pico 2 loop). With JP2 1-2
             * U10 is enabled by OE_BUS, which rises a little after E falls: wait for
             * it before driving against U10. */
            while (!OE_HIGH()) { }
            sio_hw->gpio_clr = D_MASK;
            sio_hw->gpio_oe_set = D_MASK;
            sio_hw->gpio_clr = D_MASK;
            sio_hw->gpio_oe_clr = D_MASK;
            bus_on_write(idx, (uint8_t)((prev >> PIN_D0) & 0xFF), time_us_32());
        }
        /* Counted after servicing, not in the E-rise-to-data window: keeps
         * that window free of anything but the transfer itself. */
        if (hw) bus_stats.hw_selected++; else bus_stats.fw_selected++;
    }
}
#endif
