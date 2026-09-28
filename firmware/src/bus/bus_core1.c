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
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();          /* never restored: core1 does nothing else */
    for (;;) {
        while (OE_HIGH()) { }                     /* wait for a cart cycle (OE_BUS low) */
        uint32_t in0 = sio_hw->gpio_in;
        /* ponytail: diagnostic resample ~70 ns later; use the later sample. Bench 2026-09-16 saw
         * A8 read high on ~3% of cycles; this tells settling-at-sample-time from a bad level. */
        __asm volatile("nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop" ::: "memory");
        uint32_t in  = sio_hw->gpio_in;
        uint32_t dif = ((in ^ in0) >> PIN_A0) & 0x3FFF;
        if (dif) { bus_stats.addr_resample++; bus_stats.addr_resample_bits |= dif; }
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        if (in & RW_MASK) {                       /* CoCo read */
            if (bus_drive) {
                sio_hw->gpio_clr = D_MASK;
                sio_hw->gpio_set = (uint32_t)bus_peek(idx) << PIN_D0;
                sio_hw->gpio_oe_set = D_MASK;
                while (!OE_HIGH()) { }
                sio_hw->gpio_oe_clr = D_MASK;
            } else {
                while (!OE_HIGH()) { }
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

/* Plus-W: decode during E low, act during E high. Works with JP2 in either
 * position (1-2: U15 also enables U10 for hardware-selected cycles, which is
 * consistent with what we do; 2-3: only PIN_OE_FW enables it, which is what
 * lets $FF60-$FF7F respond). */
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    for (;;) {
        while (sio_hw->gpio_in & E_MASK) { }            /* wait E low */
        while (!(sio_hw->gpio_in & Q_MASK)) { }         /* wait Q high: address valid */
        uint32_t in = sio_hw->gpio_in;
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        bool sel;
        if ((in & (CTS_MASK | SCS_MASK)) != (CTS_MASK | SCS_MASK)) {
            sel = true;
            bus_stats.hw_selected++;
        } else {
            uint16_t addr = idx | ((in & A14_MASK) ? 0x4000 : 0) | ((in & A15_MASK) ? 0x8000 : 0);
            sel = bus_fw_selected(addr, bus_fw_mask);
            if (sel) bus_stats.fw_selected++;
        }
        while (!(sio_hw->gpio_in & E_MASK)) { }         /* wait E high */
        if (!sel) continue;                             /* loop top waits for E low: the end of this cycle */
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
    }
}
#endif
