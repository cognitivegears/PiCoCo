#include "bus.h"
#include "hardware/sync.h"

/* core1: the event loop. DMA C writes one word per selected cycle into
 * bus_events, a ring of BUS_EVENTS slots that holds BUS_EV_NONE wherever no
 * unread event is (bus_engine.c fills it before DMA C starts at slot 0, and
 * never restarts it). core1 finds new events by reading the ring itself, not
 * a DMA register: a core reading DMA registers back to back delays the read
 * path's DMA by a clk now and then (Task 1). Each slot it consumes it sets
 * back to BUS_EV_NONE before moving on.
 *
 * event_lag_max: every 256 events, count the unread slots ahead (SRAM reads
 * only, no DMA register). A lap of the ring shows as BUS_EVENTS - 1 and
 * counts in event_lap.
 * SRAM only, interrupts off; bus_event and the hooks are BUS_HOT. */
BUS_HOT void bus_core1_main(void) {
    (void)save_and_disable_interrupts();
    uint32_t r = 0;
    for (;;) {
        uint32_t w = bus_events[r];
        if (w == BUS_EV_NONE) continue;
        bus_events[r] = BUS_EV_NONE;
        bus_event(w);
        r = (r + 1) & (BUS_EVENTS - 1);
        if ((r & 255) == 0) {
            uint32_t lag = 0;
            while (lag < BUS_EVENTS - 1 && bus_events[(r + lag) & (BUS_EVENTS - 1)] != BUS_EV_NONE) lag++;
            if (lag > bus_stats.event_lag_max) bus_stats.event_lag_max = lag;
            if (lag == BUS_EVENTS - 1) bus_stats.event_lap++;   /* never silent; its own counter: event_drop is core0's */
        }
    }
}
