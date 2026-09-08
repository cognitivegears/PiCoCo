#include "test.h"
#include "console.h"
#include "mode.h"
#include "dw.h"
#include "dw_store.h"
#include "bus.h"
#include "device.h"
#include "rom.h"
#include "becker.h"
#include "log.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[256];
static dw_store store;
static dw_server dw;

static char out[8192];
static size_t outn;

static void out_cb(void *ctx, const char *s) {
    (void)ctx;
    size_t l = strlen(s);
    if (outn + l >= sizeof(out)) l = sizeof(out) - outn - 1;
    memcpy(out + outn, s, l);
    outn += l;
    out[outn] = '\0';
}

/* Sector n's byte 0 is n & 0xFF, byte 1 is n >> 8, rest zero (matches the
 * fixture pattern used by test_dw_disk.c / test_dw_server.c). */
static void mk(const char *name, int nsect) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *fp = fopen(path, "wb");
    uint8_t sec[256];
    for (int n = 0; n < nsect; n++) {
        memset(sec, 0, sizeof(sec));
        sec[0] = (uint8_t)(n & 0xFF);
        sec[1] = (uint8_t)(n >> 8);
        fwrite(sec, 1, sizeof(sec), fp);
    }
    fclose(fp);
}

static void setup(void) {
    memset(out, 0, sizeof(out));
    outn = 0;
    bus_init();
    device_reset();
    rom_init();
    becker_init();
    device_init_all();
    log_init();
    mode_reset();
    mk("raw.dsk", 630);
    dw_store_posix_init(&store, g_dir);
    dw_init(&dw, &store, mode_dw_send, NULL);
    console_init(out_cb, NULL, &dw, &store);
    mode_set(MODE_DIAG);
}

TEST(unknown_is_err) {
    setup();
    ASSERT_EQ(console_exec("frob"), -1);
    ASSERT(strstr(out, "err"));
}

TEST(becker_mode_switch) {
    setup();
    ASSERT_EQ(console_exec("becker native"), 0);
    ASSERT_EQ(mode_get(), MODE_NATIVE);
    console_exec("becker off");
    ASSERT_EQ(mode_get(), MODE_DIAG);
    ASSERT_EQ(console_exec("becker sideways"), -1);
}

TEST(rom_commands) {
    setup();
    console_exec("rom pattern");
    ASSERT_EQ(bus_table[0x10], 0x10);
    console_exec("rom off");
    ASSERT_EQ(bus_table[0x10], 0xFF);
    ASSERT_EQ(console_exec("rom load nope.rom"), -1);
}

TEST(dw_mount_and_status) {
    setup();
    ASSERT_EQ(console_exec("dw mount 0 raw.dsk"), 0);
    ASSERT(dw.drives[0].mounted);
    console_exec("status");
    ASSERT(strstr(out, "raw.dsk"));
    ASSERT_EQ(console_exec("dw mount 9 raw.dsk"), -1);
    console_exec("dw eject 0");
    ASSERT(!dw.drives[0].mounted);
}

TEST(feed_splits_lines) {
    setup();
    console_feed((const uint8_t *)"becker lo", 9);
    ASSERT_EQ(mode_get(), MODE_DIAG);
    console_feed((const uint8_t *)"op\r\n", 4);
    ASSERT_EQ(mode_get(), MODE_LOOP);
}

TEST(trace_dump_format) {
    setup();
    bus_on_write(0x3F42, 0x41, 123);
    console_exec("trace dump 1");
    ASSERT(strstr(out, "123 3f42 W 41"));
}

TEST(log_level_cmd) {
    setup();
    console_exec("log dw debug");
    ASSERT_EQ(log_level(LOG_M_DW), LOG_DEBUG);
    ASSERT_EQ(console_exec("log nosuch info"), -1);
}

TEST(time_set) {
    setup();
    ASSERT_EQ(console_exec("time set 1767225600"), 0);
    ASSERT_EQ(console_exec("time"), 0);
    ASSERT(strstr(out, "1767225600"));
}

TEST(save_and_run_config_round_trip) {
    setup();
    ASSERT_EQ(console_exec("becker native"), 0);
    ASSERT_EQ(console_exec("dw mount 1 raw.dsk ro"), 0);
    ASSERT_EQ(console_exec("dw hdbdos off"), 0);
    ASSERT_EQ(console_exec("log dw debug"), 0);
    ASSERT_EQ(console_exec("save"), 0);

    mode_set(MODE_DIAG);
    dw_eject(&dw, 1);
    dw.hdbdos = true;
    log_set_level(LOG_M_DW, LOG_INFO);

    ASSERT(console_run_config() > 0);
    ASSERT_EQ(mode_get(), MODE_NATIVE);
    ASSERT(dw.drives[1].mounted && dw.drives[1].read_only);
    ASSERT(!dw.hdbdos);
    ASSERT_EQ(log_level(LOG_M_DW), LOG_DEBUG);
}

TEST(config_bad_line_continues) {
    setup();
    const char *cfg = "frob\nbecker loop\n";
    ASSERT_EQ(plat_cfg_write(cfg, strlen(cfg)), 0);
    ASSERT(console_run_config() > 0);
    ASSERT_EQ(mode_get(), MODE_LOOP);
}

TEST(capture_writes_file) {
    setup();
    ASSERT_EQ(console_exec("dw capture on cap.bin"), 0);
    mode_set(MODE_NATIVE);
    bus_on_write(0x3F42, 0x5A, 0);
    bus_on_write(0x3F42, 0x41, 0);
    mode_pump(&dw, 0);
    ASSERT_EQ(console_exec("dw capture off"), 0);

    char path[512];
    snprintf(path, sizeof(path), "%s/cap.bin", g_dir);
    FILE *fp = fopen(path, "rb");
    ASSERT(fp != NULL);
    uint8_t buf[32];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    ASSERT_EQ(n, 9);
    uint8_t want[9] = { 0, 2, 0, 0x5A, 0x41, 1, 1, 0, 0xFF };
    ASSERT_MEMEQ(buf, want, 9);
}

TEST(native_pump_end_to_end) {
    setup();
    ASSERT_EQ(console_exec("dw mount 0 raw.dsk"), 0);
    mode_set(MODE_NATIVE);
    bus_on_write(0x3F42, 0x52, 0); /* DW_OP_READ */
    uint8_t payload[4] = { 0, 0, 0, 5 }; /* drive 0, lsn 5 */
    for (int i = 0; i < 4; i++) bus_on_write(0x3F42, payload[i], 0);
    mode_pump(&dw, 0);

    bus_on_read_done(0x3F41, 0); /* status poll: publishes (single-writer rule) */
    ASSERT_EQ(bus_table[0x3F41], 2);
    ASSERT_EQ(bus_table[0x3F42], 0); /* rc */

    uint8_t popped[259];
    for (int i = 0; i < 259; i++) {
        popped[i] = bus_table[0x3F42];
        bus_on_read_done(0x3F42, 0);
        mode_pump(&dw, 0);
    }
    ASSERT_EQ(popped[0], 0);      /* rc */
    ASSERT_EQ(popped[1], 0);      /* checksum hi (sector sum = 5) */
    ASSERT_EQ(popped[2], 5);      /* checksum lo */
    ASSERT_EQ(popped[3], 5);      /* first data byte */
    ASSERT_EQ(popped[258], 0);    /* last data byte (sector is all zero past byte 0) */
    ASSERT_EQ(mode_stats.reply_overflow, 0);
    ASSERT_EQ(bus_table[0x3F41], 0); /* drained */
}

/* Two complete READ requests queued before any pump ("pipelined"; real
 * DriveWire clients wait for a reply before sending the next command, but
 * the pump must not corrupt/overflow if one doesn't). */
TEST(native_pump_backpressure) {
    setup();
    ASSERT_EQ(console_exec("dw mount 0 raw.dsk"), 0);
    mode_set(MODE_NATIVE);
    /* 12 complete READ requests (60 bytes) fit in one 64-byte becker_read()
     * batch: the actual worst case, not just two. */
    for (int r = 0; r < 12; r++) {
        uint8_t req[5] = { 0x52, 0, 0, 0, (uint8_t)r }; /* READ drive 0, lsn r */
        for (int i = 0; i < 5; i++) bus_on_write(0x3F42, req[i], 0);
    }
    mode_pump(&dw, 0);
    ASSERT_EQ(mode_stats.reply_overflow, 0);

    bus_on_read_done(0x3F41, 0);
    uint8_t popped[12 * 259];
    int n = 0;
    while (becker_stats.reads < 12 * 259) {
        popped[n++] = bus_table[0x3F42];
        bus_on_read_done(0x3F42, 0);
        mode_pump(&dw, 0);
    }
    ASSERT_EQ(becker_stats.reads, 12 * 259);
    ASSERT_EQ(mode_stats.reply_overflow, 0);
    for (int k = 0; k < 12; k++) ASSERT_EQ(popped[259 * k], 0); /* rc of reply k */
}

TEST(selftest_passes) {
    setup();
    ASSERT_EQ(console_exec("dw selftest"), 0);
    ASSERT(strstr(out, "selftest ok"));
}

TEST(bus_drive_cmd) {
    setup();
    ASSERT(!bus_drive_get());
    ASSERT_EQ(console_exec("bus drive on"), 0);
    ASSERT(bus_drive_get());
    ASSERT_EQ(console_exec("bus"), 0);
    ASSERT(strstr(out, "bus drive on"));
    ASSERT_EQ(console_exec("save"), 0);
    bus_drive_set(false);
    ASSERT(console_run_config() > 0);
    ASSERT(bus_drive_get());
    ASSERT_EQ(console_exec("bus drive sideways"), -1);
    ASSERT_EQ(console_exec("bus drive off"), 0);
    ASSERT(!bus_drive_get());
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/consoleXXXXXX", tmpdir);
    char *dir = mkdtemp(tmpl);
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    plat_host_set_dir(g_dir);

    RUN(unknown_is_err);
    RUN(becker_mode_switch);
    RUN(rom_commands);
    RUN(dw_mount_and_status);
    RUN(feed_splits_lines);
    RUN(trace_dump_format);
    RUN(log_level_cmd);
    RUN(time_set);
    RUN(save_and_run_config_round_trip);
    RUN(config_bad_line_continues);
    RUN(capture_writes_file);
    RUN(native_pump_end_to_end);
    RUN(native_pump_backpressure);
    RUN(selftest_passes);
    RUN(bus_drive_cmd);
    TEST_MAIN_END
}
