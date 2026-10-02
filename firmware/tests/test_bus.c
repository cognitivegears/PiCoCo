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

TEST(drive_flag) {
    bus_init();
    ASSERT(!bus_drive_get());
    bus_drive_set(true);
    ASSERT(bus_drive_get());
    bus_drive_set(false);
    ASSERT(!bus_drive_get());
}

static int wh_order;            /* 1 = hook ran before the event was queued */
static uint8_t wh_data;
static void whook_capture(uint8_t d) {
    uint16_t idx; uint8_t data;
    wh_data = d;
    wh_order = bus_pop_write(&idx, &data) ? 2 : 1;   /* ring must still be empty */
}
static void whook_noop(uint8_t d) { (void)d; }

TEST(write_hook_runs_before_queue_with_data) {
    bus_init();
    wh_order = 0; wh_data = 0;
    ASSERT_EQ(bus_add_write_hook(0x3F40, whook_capture), 0);
    bus_on_write(0x3F40, 0x5A, 0);
    ASSERT_EQ(wh_order, 1);                 /* ring was still empty when the hook ran */
    ASSERT_EQ(wh_data, 0x5A);
    ASSERT_EQ(bus_stats.whooks_run, 1);
    bus_on_write(0x3F41, 0x11, 0);          /* other index: no hook */
    ASSERT_EQ(bus_stats.whooks_run, 1);
    uint16_t idx; uint8_t data;
    ASSERT(bus_pop_write(&idx, &data));     /* 0x3F40 event still queued after the hook */
    ASSERT_EQ(idx, 0x3F40);
    ASSERT_EQ(data, 0x5A);
    ASSERT(bus_pop_write(&idx, &data));
    ASSERT_EQ(idx, 0x3F41);
    ASSERT(!bus_pop_write(&idx, &data));
}

TEST(write_hook_table_full) {
    bus_init();
    for (int i = 0; i < BUS_MAX_HOOKS; i++) ASSERT_EQ(bus_add_write_hook((uint16_t)i, whook_noop), 0);
    ASSERT_EQ(bus_add_write_hook(99, whook_noop), -1);
}

TEST(io_set_reaches_every_bank) {
    bus_init();
    bus_io_set(BUS_IDX_BECKER_STATUS, 0x02);
    for (int b = 0; b < BUS_BANKS; b++) ASSERT_EQ(bus_mem[b][BUS_IDX_BECKER_STATUS], 0x02);
}

TEST(peek_follows_the_bank) {
    bus_init();
    bus_mem[0][0x10] = 0xA0; bus_mem[3][0x10] = 0xA3;
    ASSERT_EQ(bus_peek(0x10), 0xA0);
    bus_bank = 3;
    ASSERT_EQ(bus_peek(0x10), 0xA3);
    bus_bank = 0;
}

TEST(init_fills_every_bank) {
    bus_mem[5][0x123] = 0x00; bus_bank = 2;
    bus_init();
    ASSERT_EQ(bus_mem[5][0x123], 0xFF);
    ASSERT_EQ(bus_bank, 0);
}

TEST(set_read_on_io_entry_mirrors) {
    bus_init();
    bus_set_read(BUS_IO_LO + 3, 0x5C);
    for (int b = 0; b < BUS_BANKS; b++) ASSERT_EQ(bus_mem[b][BUS_IO_LO + 3], 0x5C);
    bus_set_read(0x20, 0x11);                 /* ROM window: bank 0 only */
    ASSERT_EQ(bus_mem[0][0x20], 0x11);
    ASSERT_EQ(bus_mem[1][0x20], 0xFF);
}

TEST(stats_new_fields_zeroed) {
    bus_stats.whooks_run = 5;
    bus_init();
    ASSERT_EQ(bus_stats.whooks_run, 0);
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
    RUN(drive_flag);
    RUN(write_hook_runs_before_queue_with_data);
    RUN(write_hook_table_full);
    RUN(stats_new_fields_zeroed);
    RUN(io_set_reaches_every_bank);
    RUN(peek_follows_the_bank);
    RUN(init_fills_every_bank);
    RUN(set_read_on_io_entry_mirrors);
    TEST_MAIN_END
}
