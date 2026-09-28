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
