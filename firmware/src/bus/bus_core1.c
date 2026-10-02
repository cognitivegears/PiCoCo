#include "bus.h"
#include "hardware/sync.h"

/* core1 entry. The read engine (bus_engine.c) needs no CPU; core1 idles
 * until the event loop (writes, read hooks, trace) lands here. */
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    for (;;) __wfe();
}
