#include "test.h"
#include "bus.h"
#include "device.h"
#include "rom.h"
#include "becker.h"
#include "dw_store.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void setup(void) {
    bus_init();
    device_reset();
    rom_init();
    becker_init();
    device_init_all();
}

TEST(status_follows_queue) {
    setup();
    ASSERT_EQ(bus_table[0x3F41], 0);
    ASSERT_EQ(bus_table[0x3F42], 0xFF);
    uint8_t b = 0x41;
    ASSERT_EQ(becker_write(&b, 1), 1);
    ASSERT_EQ(bus_table[0x3F41], 2);
    ASSERT_EQ(bus_table[0x3F42], 0x41);
}

TEST(read_hook_pops_one) {
    setup();
    becker_write((const uint8_t *)"AB", 2);
    bus_on_read_done(0x3F42, 0);
    ASSERT_EQ(bus_table[0x3F42], 'B');
    bus_on_read_done(0x3F42, 0);
    ASSERT_EQ(bus_table[0x3F41], 0);
    ASSERT_EQ(becker_stats.reads, 2);
    bus_on_read_done(0x3F42, 0);
    ASSERT_EQ(becker_stats.underrun, 1);
}

TEST(status_read_does_not_pop) {
    setup();
    becker_write((const uint8_t *)"A", 1);
    bus_on_read_done(0x3F41, 0);
    ASSERT_EQ(bus_table[0x3F42], 'A');
}

TEST(coco_write_reaches_stream) {
    setup();
    bus_on_write(0x3F42, 0x52, 0);
    bus_on_write(0x3F41, 0x99, 0);
    ASSERT_EQ(device_dispatch_writes(), 2);
    uint8_t buf[4];
    ASSERT_EQ(becker_read(buf, 4), 1);
    ASSERT_EQ(buf[0], 0x52);
    ASSERT_EQ(becker_stats.writes, 1);
}

TEST(tx_backpressure) {
    setup();
    uint8_t big[300];
    memset(big, 0, sizeof(big));
    ASSERT_EQ(becker_write(big, 300), 255);
    ASSERT_EQ(becker_tx_free(), 0);
}

TEST(rx_overrun_counted) {
    setup();
    for (int i = 0; i < 1100; i++) {
        bus_on_write(0x3F42, 1, 0);
        device_dispatch_writes();
    }
    ASSERT(becker_stats.overrun > 0);
    ASSERT_EQ(becker_rx_avail(), 1023);
}

TEST(loopback) {
    setup();
    bus_on_write(0x3F42, 65, 0);
    device_dispatch_writes();
    becker_loopback_pump();
    ASSERT_EQ(bus_table[0x3F42], 65);
}

TEST(rom_pattern_and_off) {
    setup();
    rom_pattern();
    ASSERT_EQ(bus_table[0x1234], 0x34);
    ASSERT_EQ(bus_table[0x2000], 0xFF);
    rom_off();
    ASSERT_EQ(bus_table[0x1234], 0xFF);
}

TEST(rom_16k_keeps_becker) {
    setup();
    static uint8_t img[16384];
    memset(img, 0x11, sizeof(img));
    becker_write((const uint8_t *)"Z", 1);
    ASSERT_EQ(rom_load_mem(img, 16384), 0);
    ASSERT_EQ(bus_table[0x3EFF], 0x11);
    ASSERT_EQ(bus_table[0x3F42], 'Z');
    ASSERT_EQ(rom_load_mem(img, 100), -2);
}

TEST(rom_load_file_test) {
    setup();
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/romfileXXXXXX", tmpdir);
    char *dir = mkdtemp(tmpl);
    dw_store st;
    dw_store_posix_init(&st, dir);

    char path[512];
    snprintf(path, sizeof(path), "%s/r.rom", dir);
    uint8_t buf[8192];
    memset(buf, 0x22, sizeof(buf));
    FILE *fp = fopen(path, "wb");
    fwrite(buf, 1, sizeof(buf), fp);
    fclose(fp);

    ASSERT_EQ(rom_load_file(&st, "r.rom"), 0);
    ASSERT_EQ(bus_table[0], 0x22);
    ASSERT_EQ(rom_load_file(&st, "x"), -1);
}

TEST(dispatch_ignores_unowned) {
    setup();
    bus_on_write(0x1000, 1, 0);
    ASSERT_EQ(device_dispatch_writes(), 1);
    ASSERT_EQ(becker_stats.writes, 0);
}

int main(void) {
    RUN(status_follows_queue);
    RUN(read_hook_pops_one);
    RUN(status_read_does_not_pop);
    RUN(coco_write_reaches_stream);
    RUN(tx_backpressure);
    RUN(rx_overrun_counted);
    RUN(loopback);
    RUN(rom_pattern_and_off);
    RUN(rom_16k_keeps_becker);
    RUN(rom_load_file_test);
    RUN(dispatch_ignores_unowned);
    TEST_MAIN_END
}
