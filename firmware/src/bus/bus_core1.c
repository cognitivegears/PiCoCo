#include "bus.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/timer.h"
#include PICOCO_BOARD_H

#define OE_MASK (1u << PIN_OE_BUS)
#define RW_MASK (1u << PIN_RW)
#define D_MASK  (0xFFu << PIN_D0)

/* core1 entry: launched once from main.c after config replay, never returns.
 * Flash-free per Plan B - everything reachable from here must be BUS_HOT or
 * static inline (see the nm/objdump acceptance check in the task report). */
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();          /* never restored: core1 does nothing else */
    for (;;) {
        while (sio_hw->gpio_in & OE_MASK) { }     /* wait for a cart cycle (OE_BUS low) */
        uint32_t in  = sio_hw->gpio_in;
        uint16_t idx = (in >> PIN_A0) & 0x3FFF;
        if (in & RW_MASK) {                       /* CoCo read */
            if (bus_drive) {
                sio_hw->gpio_clr = D_MASK;
                sio_hw->gpio_set = (uint32_t)bus_table[idx] << PIN_D0;
                sio_hw->gpio_oe_set = D_MASK;
                while (!(sio_hw->gpio_in & OE_MASK)) { }
                sio_hw->gpio_oe_clr = D_MASK;
            } else {
                while (!(sio_hw->gpio_in & OE_MASK)) { }
            }
            bus_on_read_done(idx, time_us_32());
        } else {                                  /* CoCo write: last sample before OE_BUS rises */
            uint32_t d;
            do { d = sio_hw->gpio_in; } while (!(d & OE_MASK));
            bus_on_write(idx, (uint8_t)((d >> PIN_D0) & 0xFF), time_us_32());
        }
    }
}
