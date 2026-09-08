#include "test.h"
#include "bus.h"

static int hook_n;
static void hook_incr(void) { hook_n++; }
static void hook_noop(void) { }

TEST(table_defaults_ff) {
    bus_init();
    ASSERT_EQ(bus_table[0], 0xFF);
    ASSERT_EQ(bus_table[BUS_TABLE_SIZE - 1], 0xFF);
}

TEST(set_read_and_range_clipped) {
    bus_init();
    uint8_t p[4] = {1, 2, 3, 4};
    bus_set_read_range(BUS_TABLE_SIZE - 2, p, 4);
    ASSERT_EQ(bus_table[BUS_TABLE_SIZE - 2], 1);
    ASSERT_EQ(bus_table[BUS_TABLE_SIZE - 1], 2);
    bus_set_read(7, 0x42);
    ASSERT_EQ(bus_table[7], 0x42);
}

TEST(write_events_fifo_and_overrun) {
    bus_init();
    for (int i = 0; i < 256; i++) bus_on_write((uint16_t)i, (uint8_t)i, (uint32_t)i);
    ASSERT_EQ(bus_stats.write_overrun, 1);
    uint16_t idx; uint8_t data;
    for (int i = 0; i < 255; i++) {
        ASSERT(bus_pop_write(&idx, &data));
        ASSERT_EQ(idx, i);
        ASSERT_EQ(data, i);
    }
    ASSERT(!bus_pop_write(&idx, &data));
}

TEST(read_hook_runs_only_for_its_index) {
    bus_init();
    hook_n = 0;
    bus_add_read_hook(0x3F42, hook_incr);
    bus_on_read_done(0x3F41, 0);
    ASSERT_EQ(hook_n, 0);
    bus_on_read_done(0x3F42, 0);
    ASSERT_EQ(hook_n, 1);
}

TEST(hook_table_full) {
    bus_init();
    ASSERT_EQ(bus_add_read_hook(1, hook_noop), 0);
    ASSERT_EQ(bus_add_read_hook(2, hook_noop), 0);
    ASSERT_EQ(bus_add_read_hook(3, hook_noop), 0);
    ASSERT_EQ(bus_add_read_hook(4, hook_noop), 0);
    ASSERT_EQ(bus_add_read_hook(5, hook_noop), -1);
}

TEST(trace_records_and_wraps) {
    bus_init();
    for (int i = 0; i < BUS_TRACE_SIZE + 10; i++) bus_on_write((uint16_t)(i & 0x3FFF), 0, (uint32_t)i);
    bus_trace_entry e[BUS_TRACE_SIZE];
    size_t n = bus_trace_copy(e, BUS_TRACE_SIZE);
    ASSERT_EQ(n, BUS_TRACE_SIZE);
    ASSERT_EQ(e[0].t_us, 10);
    ASSERT_EQ(e[n - 1].t_us, BUS_TRACE_SIZE + 9);
    ASSERT_EQ(e[0].rw, 0);
}

TEST(trace_freeze) {
    bus_init();
    bus_on_read_done(1, 5);
    bus_trace_freeze(true);
    bus_on_read_done(2, 6);
    bus_trace_entry e[10];
    size_t n = bus_trace_copy(e, 10);
    ASSERT_EQ(n, 1);
    ASSERT_EQ(e[0].rw, 1);
    ASSERT_EQ(e[0].data, 0xFF);
}

TEST(stats) {
    bus_init();
    bus_on_read_done(1, 0);
    bus_on_write(1, 0, 0);
    ASSERT_EQ(bus_stats.cycles, 2);
    ASSERT_EQ(bus_stats.reads, 1);
    ASSERT_EQ(bus_stats.writes, 1);
}

int main(void) {
    RUN(table_defaults_ff);
    RUN(set_read_and_range_clipped);
    RUN(write_events_fifo_and_overrun);
    RUN(read_hook_runs_only_for_its_index);
    RUN(hook_table_full);
    RUN(trace_records_and_wraps);
    RUN(trace_freeze);
    RUN(stats);
    TEST_MAIN_END
}
