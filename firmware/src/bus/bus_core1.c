#include "bus.h"
#include "bus_engine.h"
#include "hardware/sync.h"

/* core1: the event loop. DMA C writes one word per selected cycle into
 * bus_events, a ring of BUS_EVENTS slots that holds BUS_EV_NONE wherever no
 * unread event is (bus_engine.c fills it before DMA C starts at slot 0, and
 * never restarts it). core1 finds new events by reading the ring itself, not
 * a DMA register: a core reading DMA registers back to back delays the read
 * path's DMA by a clk now and then (Task 1); only a suspected lap reads one.
 * Each slot it consumes it sets back to BUS_EV_NONE before moving on.
 *
 * event_lag_max: every 256 events, count the unread slots ahead (SRAM reads
 * only, no DMA register), up to BUS_EV_LAG_CAP.
 * ponytail: capped so the scan stays cheap while core1 is behind (uncapped it
 * cost up to 2047 reads per 256 events, enough to keep a lapped core1 from
 * ever catching up); lag max reads the cap for anything from it up.
 * At the cap, check for a lap and resync (resync below).
 * SRAM only, interrupts off; bus_event and the hooks are BUS_HOT. */
volatile bool bus_core1_hold;

/* A lap: the slot DMA C writes next still holds an unread event (behind but
 * not lapped, core1 cleared it on its last pass; a moving writer is left to
 * the next check). Then the ring is out of order and every slot is stale:
 * empty them all, forward from the writer, which core1 outruns, then the ones
 * DMA C wrote meanwhile, and go on from where it is now. Those events and
 * everything unread are dropped, counted once in event_lap. Before a check
 * finds the lap, core1 serves up to 255 stale slots out of order (256 more
 * for each check that leaves a moving writer to the next); after detection,
 * none. */
static BUS_HOT uint32_t resync(uint32_t r) {
    uint32_t w = bus_engine_event_pos();
    if (bus_events[w] == BUS_EV_NONE || bus_engine_event_pos() != w) return r;
    for (uint32_t i = 0; i < BUS_EVENTS; i++) bus_events[(w + i) & (BUS_EVENTS - 1)] = BUS_EV_NONE;
    uint32_t now = bus_engine_event_pos();
    for (; w != now; w = (w + 1) & (BUS_EVENTS - 1)) bus_events[w] = BUS_EV_NONE;
    bus_stats.event_lap++;                           /* never silent; its own counter: event_drop is core0's */
    return now;
}

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
            while (bus_core1_hold) { }               /* bus selftest lap */
            uint32_t lag = 0;
            while (lag < BUS_EV_LAG_CAP && bus_events[(r + lag) & (BUS_EVENTS - 1)] != BUS_EV_NONE) lag++;
            if (lag > bus_stats.event_lag_max) bus_stats.event_lag_max = lag;
            if (lag == BUS_EV_LAG_CAP) r = resync(r);
        }
    }
}
