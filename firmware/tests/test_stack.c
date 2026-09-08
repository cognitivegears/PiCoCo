/* End-to-end stack test: virtual CoCo (sim_bus) driving the real bus_table,
 * write ring and hooks through console/dw/becker exactly as core1 will. */
#include "test.h"
#include "sim_bus.h"
#include "console.h"
#include "mode.h"
#include "dw.h"
#include "dw_store.h"
#include "dw_util.h"
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

/* Sector n's byte 0 is n & 0xFF, byte 1 is n >> 8, rest zero (matches
 * test_console.c / test_dw_disk.c / test_dw_server.c). */
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
    mk("s.dsk", 630);
    dw_store_posix_init(&store, g_dir);
    dw_init(&dw, &store, mode_dw_send, NULL);
    console_init(out_cb, NULL, &dw, &store);
    console_exec("dw mount 0 raw.dsk");
    console_exec("dw mount 1 s.dsk");
    console_exec("dw hdbdos off");
    mode_set(MODE_NATIVE);
}

static void pc(uint8_t b) { sim_becker_putc(&dw, b); }
static int gc(void) { return sim_becker_getc(&dw, 1000); }

TEST(rom_pattern_visible_to_coco) {
    setup();
    console_exec("rom pattern");
    ASSERT_EQ(sim_read(0xC123), 0x23);
    ASSERT_EQ(sim_read(0xE000), 0xFF);
}

TEST(coco_readex_sector) {
    setup();
    pc(0xD2); pc(0); pc(0); pc(0); pc(9); /* READEX drive 0 lsn 9 */
    uint8_t d[256];
    for (int i = 0; i < 256; i++) {
        int v = gc();
        ASSERT(v >= 0);
        d[i] = (uint8_t)v;
    }
    ASSERT_EQ(d[0], 9);
    uint16_t sum = dw_checksum(d, 256);
    pc((uint8_t)(sum >> 8));
    pc((uint8_t)(sum & 0xFF));
    int rc = gc();
    ASSERT(rc >= 0);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(sim_read(0xFF41), 0);
}

TEST(coco_read_259) {
    setup();
    pc(0x52); pc(0); pc(0); pc(0); pc(9); /* READ drive 0 lsn 9 */
    int rc = gc(); ASSERT(rc >= 0); ASSERT_EQ(rc, 0);
    int hi = gc(); ASSERT(hi >= 0);
    int lo = gc(); ASSERT(lo >= 0);
    uint8_t d[256];
    for (int i = 0; i < 256; i++) {
        int v = gc();
        ASSERT(v >= 0);
        d[i] = (uint8_t)v;
    }
    ASSERT_EQ(d[0], 9);
    ASSERT_EQ((hi << 8) | lo, dw_checksum(d, 256));
}

TEST(coco_write_then_read_back) {
    setup();
    pc(0x57); pc(1); pc(0); pc(0); pc(4); /* WRITE drive 1 lsn 4 */
    uint8_t d[256];
    memset(d, 0x3C, sizeof(d));
    for (int i = 0; i < 256; i++) pc(d[i]);
    uint16_t sum = dw_checksum(d, 256);
    pc((uint8_t)(sum >> 8));
    pc((uint8_t)(sum & 0xFF));
    int rc = gc(); ASSERT(rc >= 0); ASSERT_EQ(rc, 0);

    pc(0xD2); pc(1); pc(0); pc(0); pc(4); /* READEX drive 1 lsn 4 */
    uint8_t back[256];
    for (int i = 0; i < 256; i++) {
        int v = gc();
        ASSERT(v >= 0);
        back[i] = (uint8_t)v;
    }
    for (int i = 0; i < 256; i++) ASSERT_EQ(back[i], 0x3C);
    uint16_t sum2 = dw_checksum(back, 256);
    pc((uint8_t)(sum2 >> 8));
    pc((uint8_t)(sum2 & 0xFF));
    int rc2 = gc(); ASSERT(rc2 >= 0); ASSERT_EQ(rc2, 0);
}

TEST(coco_bad_checksum_gets_crc) {
    setup();
    pc(0xD2); pc(0); pc(0); pc(0); pc(9); /* READEX drive 0 lsn 9 */
    uint8_t d[256];
    for (int i = 0; i < 256; i++) {
        int v = gc();
        ASSERT(v >= 0);
        d[i] = (uint8_t)v;
    }
    uint16_t bad = (uint16_t)(dw_checksum(d, 256) ^ 0xFFFF);
    pc((uint8_t)(bad >> 8));
    pc((uint8_t)(bad & 0xFF));
    int rc = gc();
    ASSERT(rc >= 0);
    ASSERT_EQ(rc, DW_E_CRC);
}

TEST(unmounted_drive) {
    setup();
    pc(0xD2); pc(3); pc(0); pc(0); pc(0); /* READEX drive 3 (never mounted) */
    uint8_t d[256];
    for (int i = 0; i < 256; i++) {
        int v = gc();
        ASSERT(v >= 0);
        d[i] = (uint8_t)v;
    }
    for (int i = 0; i < 256; i++) ASSERT_EQ(d[i], 0);
    uint16_t sum = dw_checksum(d, 256);
    pc((uint8_t)(sum >> 8));
    pc((uint8_t)(sum & 0xFF));
    int rc = gc();
    ASSERT(rc >= 0);
    ASSERT_EQ(rc, DW_E_NOTRDY);
}

TEST(loopback_mode) {
    setup();
    mode_set(MODE_LOOP);
    sim_write(0xFF42, 0x77);
    mode_pump(&dw, 0);
    sim_read(0xFF41); /* first poll returns stale 0 and publishes */
    ASSERT_EQ(sim_read(0xFF41), 2);
    ASSERT_EQ(sim_read(0xFF42), 0x77);
}

TEST(status_when_idle_is_zero_and_data_ff) {
    setup();
    ASSERT_EQ(sim_read(0xFF41), 0);
    ASSERT_EQ(sim_read(0xFF42), 0xFF);
    ASSERT(becker_stats.underrun >= 1);
}

TEST(trace_shows_transaction) {
    setup();
    pc(0xD2); pc(0); pc(0); pc(0); pc(9); /* READEX drive 0 lsn 9 */
    uint8_t d[256];
    for (int i = 0; i < 256; i++) {
        int v = gc();
        ASSERT(v >= 0);
        d[i] = (uint8_t)v;
    }
    uint16_t sum = dw_checksum(d, 256);
    pc((uint8_t)(sum >> 8));
    pc((uint8_t)(sum & 0xFF));
    int rc = gc();
    ASSERT(rc >= 0);

    bus_trace_entry e[8];
    size_t n = bus_trace_copy(e, 8);
    ASSERT(n > 0);
    ASSERT_EQ(e[n - 1].idx, 0x3F42);
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/stackXXXXXX", tmpdir);
    char *dir = mkdtemp(tmpl);
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    plat_host_set_dir(g_dir);

    RUN(rom_pattern_visible_to_coco);
    RUN(coco_readex_sector);
    RUN(coco_read_259);
    RUN(coco_write_then_read_back);
    RUN(coco_bad_checksum_gets_crc);
    RUN(unmounted_drive);
    RUN(loopback_mode);
    RUN(status_when_idle_is_zero_and_data_ff);
    RUN(trace_shows_transaction);
    TEST_MAIN_END
}
