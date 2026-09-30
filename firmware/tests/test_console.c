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
#include "net.h"
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

static uint32_t file_size(const char *name) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", g_dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (uint32_t)n;
}

static void setup(void) {
    net_forget();
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
    ASSERT(strstr(out, "dw hdbdos on"));
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

/* cmd_save must write "dw disk insert" (which re-joins its raw tail) for a
 * read-write mount, not "dw mount" (one token): "my disk.dsk" would replay
 * as "dw mount 0 my" and fail. */
TEST(save_preserves_rw_name_with_spaces) {
    setup();
    mk("saved rw.dsk", 10);
    ASSERT_EQ(console_exec("dw disk insert 0 saved rw.dsk"), 0);
    ASSERT_EQ(console_exec("save"), 0);

    dw_eject(&dw, 0);
    ASSERT(!dw.drives[0].mounted);

    ASSERT(console_run_config() > 0);
    ASSERT(dw.drives[0].mounted);
    ASSERT(!dw.drives[0].read_only);
    ASSERT(strcmp(dw.drives[0].name, "saved rw.dsk") == 0);
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
    uint8_t want[9] = { 0, 2, 0, 0x5A, 0x41, 1, 1, 0, 0x04 };
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

static char rbuf[4096];
static size_t rn;
static int remote(const char *line) {
    memset(rbuf, 0, sizeof(rbuf));
    rn = 0;
    return console_exec_remote(NULL, line, rbuf, sizeof(rbuf) - 1, &rn);
}

TEST(time_reports_clock_lost_on_host) {
    setup();
    ASSERT_EQ(remote("time"), 0);
    ASSERT(strstr(rbuf, "clock lost\n"));
}

TEST(disk_show_format) {
    setup();
    mk("b.dsk", 630);
    ASSERT_EQ(console_exec("dw mount 0 b.dsk"), 0);
    ASSERT_EQ(console_exec("dw mount 2 raw.dsk ro"), 0);
    outn = 0; out[0] = 0;
    ASSERT_EQ(console_exec("dw disk show"), 0);
    ASSERT(strstr(out, "\r\nCurrent DriveWire disks:\r\n\r\nX0   b.dsk\r\nX2  *raw.dsk\r\n"));
    outn = 0; out[0] = 0;
    ASSERT_EQ(console_exec("dw disk show 2"), 0);
    ASSERT(strstr(out, "Details for disk in drive #2:\r\n\r\nraw.dsk\r\n"));
    ASSERT_EQ(console_exec("dw disk show 1"), -1);
}

TEST(disk_insert_and_eject) {
    setup();
    ASSERT_EQ(remote("dw disk insert 1 raw.dsk"), 0);
    ASSERT(strcmp(rbuf, "Disk inserted in drive 1.") == 0);
    ASSERT(dw.drives[1].mounted);
    ASSERT_EQ(remote("dw disk eject 1"), 0);
    ASSERT(strcmp(rbuf, "Disk ejected from drive 1.\r\n") == 0);
    ASSERT(!dw.drives[1].mounted);
    ASSERT_EQ(remote("dw disk eject 1"), 255);
    ASSERT(strcmp(rbuf, "drive not loaded") == 0);
    ASSERT_EQ(remote("dw disk insert x raw.dsk"), 101);
    ASSERT_EQ(remote("dw disk insert 4 raw.dsk"), 101);
    ASSERT_EQ(remote("dw disk"), 10);
}

TEST(disk_insert_name_with_spaces) {
    setup();
    mk("my game disk.dsk", 630);
    ASSERT_EQ(remote("dw disk insert 0 my game disk.dsk  "), 0);
    ASSERT(strcmp(dw.drives[0].name, "my game disk.dsk") == 0);
}

TEST(disk_insert_long_name_fails) {
    setup();
    mk("a_really_long_disk_image_name_over_32.dsk", 630);
    ASSERT_EQ(remote("dw disk insert 0 a_really_long_disk_image_name_over_32.dsk"), 255);
    ASSERT(strcmp(rbuf, "mount failed") == 0);
}

TEST(disk_insert_replaces_mounted) {
    setup();
    mk("b.dsk", 630);
    ASSERT_EQ(console_exec("dw mount 0 raw.dsk"), 0);
    ASSERT_EQ(remote("dw disk insert 0 b.dsk"), 0);
    ASSERT(strcmp(dw.drives[0].name, "b.dsk") == 0);
}

TEST(remote_allowed_commands_run) {
    setup();
    ASSERT_EQ(remote("version"), 0);
    ASSERT(strstr(rbuf, "version"));
    ASSERT(!strstr(rbuf, "ok\n"));          /* no console "ok" terminator */
    ASSERT_EQ(remote("VERSION"), 0);        /* case-insensitive */
    ASSERT(strstr(rbuf, "version"));
    ASSERT_EQ(remote("fs ls"), 0);
    ASSERT(strstr(rbuf, "raw.dsk"));
    ASSERT_EQ(remote("dw mount 0 raw.dsk"), 0);
    ASSERT(dw.drives[0].mounted);
    ASSERT_EQ(outn, 0);                     /* nothing leaked to the USB console */
}

TEST(remote_refuses_console_only) {
    setup();
    const char *deny[] = { "smoke", "halt on", "bus drive off", "becker off", "becker net", "fs rm raw.dsk",
                           "fs format", "fs export", "rom load x.rom", "rom pattern", "trace dump",
                           "crash", "log dw debug", "stats reset", "dw capture off", "dw selftest",
                           "reboot", "bootsel", "frob",
                           "fs", "dw", "rom", "statusx", "fsls", "FS RM raw.dsk" };
    for (size_t i = 0; i < sizeof(deny) / sizeof(deny[0]); i++) {
        ASSERT_EQ(remote(deny[i]), 255);
        ASSERT(strcmp(rbuf, "console only") == 0);
    }
    ASSERT_EQ(mode_get(), MODE_DIAG);
}

TEST(remote_refuses_mounting_config) {
    setup();
    mk("picoco.cfg", 1);
    mk("picoco.dsk", 1);
    const char *deny[] = { "dw mount 0 PICOCO.CFG", "dw mount 0 picoco.cfg.",
                            "dw mount 0 picoco.cfg ", "dw mount 0 /picoco.cfg" };
    for (size_t i = 0; i < sizeof(deny) / sizeof(deny[0]); i++) {
        ASSERT_EQ(remote(deny[i]), 255);
        ASSERT(!dw.drives[0].mounted);
    }
    ASSERT_EQ(console_exec("dw mount 0 picoco.cfg"), -1);  /* USB path refuses it too */
    ASSERT(!dw.drives[0].mounted);
    ASSERT_EQ(remote("dw mount 0 picoco.dsk"), 0);         /* an ordinary name still mounts */
    ASSERT(dw.drives[0].mounted);
}

/* FatFS ends a path at the first byte < 0x20 and drops a trailing
 * separator (ff.c ~2900), so "picoco.cfg\" opens picoco.cfg on the Pico
 * even though its last path component (split on '\\') is empty, not
 * "picoco.cfg". dw_disk_name_ok must refuse it outright. */
TEST(remote_refuses_config_bypass_backslash) {
    setup();
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "dw disk insert 0 picoco.cfg\\");
    ASSERT_EQ(remote(cmd), 255);
    ASSERT(!dw.drives[0].mounted);
}

TEST(remote_error_codes) {
    setup();
    ASSERT_EQ(remote("dw hdbdos"), 10);
    ASSERT(strncmp(rbuf, "usage", 5) == 0);
    ASSERT(!strchr(rbuf, '\n'));
    ASSERT_EQ(remote("dw eject 9"), 101);
    ASSERT(strcmp(rbuf, "bad drive") == 0);
    ASSERT_EQ(remote("dw mount 0 nope.dsk"), 255);
    ASSERT(strcmp(rbuf, "mount failed") == 0);
}

TEST(remote_output_truncates) {
    setup();
    for (int i = 0; i < 200; i++) {
        char n[64];
        snprintf(n, sizeof(n), "file_with_a_long_name_%03d.dsk", i);
        mk(n, 1);
    }
    char small[256];
    size_t sn = 0;
    ASSERT_EQ(console_exec_remote(NULL, "fs ls", small, sizeof(small), &sn), 0);
    ASSERT(sn <= sizeof(small));
    ASSERT_MEMEQ(small + sn - 4, "...\n", 4);
    ASSERT(sn == 4 || small[sn - 5] == '\n');  /* marker is its own line */
}

TEST(remote_tiny_cap_no_crash) {
    setup();
    char tiny[4];
    size_t tn = 99;
    ASSERT_EQ(console_exec_remote(NULL, "version", tiny, sizeof(tiny), &tn), 255);
    ASSERT_EQ(tn, 0);
}

TEST(remote_via_vserial_end_to_end) {
    setup();
    uint8_t pkt[] = { 0xC4, 1, 0x29, 0x64, 1, 8, 'v', 'e', 'r', 's', 'i', 'o', 'n', '\r' };
    dw_feed(&dw, pkt, sizeof(pkt), 0);
    ASSERT_EQ(dw.vser.qlen > 23, 1);
    ASSERT_MEMEQ(dw.vser.q + dw.vser.qhead, "OK command successful\n\rversion ", 31);
}

TEST(fs_new_makes_formatted_rsdos_image) {
    setup();
    ASSERT_EQ(remote("fs new blank.dsk"), 0);
    ASSERT_EQ(file_size("blank.dsk"), 161280);
    char p[512];
    snprintf(p, sizeof(p), "%s/blank.dsk", g_dir);
    FILE *f = fopen(p, "rb");
    static uint8_t img[161280];
    ASSERT_EQ(fread(img, 1, sizeof(img), f), sizeof(img));
    fclose(f);
    for (uint32_t i = 0; i < sizeof(img); i++) {
        /* Matches ToolShed's `decb dskini` (which mirrors real DSKINI):
         * track 17 sector 1 (0x13200..0x132FF, unused by RS-DOS -- the
         * directory itself is sectors 3-11) is entirely zero, and the FAT
         * sector's (0x13300) tail past the 68 granule-status bytes is zero
         * too. */
        uint8_t want = (i >= 0x13200 && i < 0x13300) ? 0x00 :
                       (i >= 0x13300 + 68 && i < 0x13400) ? 0x00 : 0xFF;
        if (img[i] != want) { printf("  byte %u = %02x\n", i, img[i]); ASSERT(0); }
    }
    ASSERT_EQ(remote("dw disk insert 0 blank.dsk"), 0);
}

TEST(fs_new_refuses_existing) {
    setup();
    ASSERT_EQ(remote("fs new raw.dsk"), 255);
    ASSERT(strcmp(rbuf, "file exists") == 0);
    ASSERT_EQ(file_size("raw.dsk"), 630 * 256);
    ASSERT_EQ(remote("fs new"), 10);
    ASSERT_EQ(remote("fs new a/b.dsk"), 255);
}

/* Case-insensitive on macOS/FAT, so "PICOCO.CFG" may already resolve to a
 * file another test left behind in the shared g_dir; assert refusal and
 * that this call did not write a fresh blank image over whatever is there,
 * rather than assuming the file starts out absent. */
TEST(fs_new_refuses_config_name) {
    setup();
    uint32_t before = file_size("PICOCO.CFG");
    ASSERT_EQ(remote("fs new PICOCO.CFG"), 255);
    ASSERT(strcmp(rbuf, "reserved name") == 0);
    ASSERT_EQ(file_size("PICOCO.CFG"), before);
    ASSERT(before != 161280);
}

/* FatFS (third_party/fatfs/ff.c IsSeparator) treats '\\' as a path
 * separator too, so on the Pico "\picoco.cfg" would otherwise bypass the
 * config guard (dw_disk_is_config_name only checked '/' and ':'). These
 * run before any store access, so the refusal shows up on the host too. */
TEST(fs_new_refuses_backslash_config) {
    setup();
    ASSERT_EQ(remote("fs new \\picoco.cfg"), 255);
    ASSERT(strcmp(rbuf, "bad name") == 0 || strcmp(rbuf, "reserved name") == 0);
}

TEST(dw_mount_refuses_backslash_config) {
    setup();
    ASSERT_EQ(remote("dw mount 0 \\picoco.cfg"), 255);
    ASSERT(!dw.drives[0].mounted);
}

/* "fs new" must take the rest of the raw line, like "dw disk insert" does,
 * so a name with spaces isn't silently truncated to its first token. */
TEST(fs_new_uses_full_raw_name) {
    setup();
    ASSERT_EQ(remote("fs new my disk.dsk"), 0);
    ASSERT_EQ(file_size("my disk.dsk"), 161280);
    ASSERT_EQ(file_size("my"), 0);
}

TEST(fs_new_refuses_long_name) {
    setup();
    char name[40];
    memset(name, 'a', 32);
    name[32] = '\0';
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "fs new %s", name);
    ASSERT_EQ(remote(cmd), 255);
    ASSERT(strcmp(rbuf, "name too long") == 0);
    ASSERT_EQ(file_size(name), 0);
}

TEST(rom_boot_records_without_loading) {
    setup();
    static uint8_t rom[8192];
    memset(rom, 0xAB, sizeof(rom));
    char p[512];
    snprintf(p, sizeof(p), "%s/next.rom", g_dir);
    FILE *f = fopen(p, "wb");
    fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
    console_exec("rom off");
    ASSERT_EQ(remote("rom boot next.rom"), 0);
    ASSERT_EQ(bus_table[0x10], 0xFF);                 /* nothing swapped live */
    ASSERT_EQ(remote("status"), 0);
    ASSERT(strstr(rbuf, "rom now none\n"));
    ASSERT(strstr(rbuf, "rom next load next.rom\n"));
    ASSERT_EQ(remote("rom boot raw.dsk"), 255);       /* wrong size */
    ASSERT_EQ(remote("rom boot nope.rom"), 255);
    ASSERT_EQ(console_exec("save"), 0);
    char cfg[1024];
    int n = plat_cfg_read(cfg, sizeof(cfg) - 1);
    ASSERT(n > 0);
    cfg[n] = 0;
    ASSERT(strstr(cfg, "rom load next.rom\n"));
}

TEST(version_is_1_3) {
    setup();
    ASSERT_EQ(remote("version"), 0);
    ASSERT(strcmp(rbuf, "version 1.3\n") == 0);
}

TEST(bus_selftest_is_pico_only_on_host) {
    setup();
    outn = 0;
    ASSERT_EQ(console_exec("bus selftest"), -1);
    ASSERT(strstr(out, "pico only"));
}

TEST(cart_command_and_save) {
    setup();
    ASSERT_EQ(console_exec("cart on"), 0);
    ASSERT_EQ(rom_cart_get(), CART_ON);
    ASSERT_EQ(console_exec("cart off"), 0);
    ASSERT_EQ(rom_cart_get(), CART_OFF);
    ASSERT_EQ(console_exec("cart auto"), 0);
    ASSERT_EQ(rom_cart_get(), CART_AUTO);
    ASSERT_EQ(console_exec("cart sideways"), -1);
    console_exec("cart on");
    outn = 0;
    ASSERT_EQ(console_exec("save"), 0);
    char cfg[1024]; int n = plat_cfg_read(cfg, sizeof cfg - 1); ASSERT(n > 0); cfg[n] = 0;
    ASSERT(strstr(cfg, "cart on\n"));
    console_exec("cart auto");
    ASSERT_EQ(console_exec("save"), 0);
    n = plat_cfg_read(cfg, sizeof cfg - 1); cfg[n] = 0;
    ASSERT(!strstr(cfg, "cart "));              /* default is omitted */
}

TEST(net_join_keeps_spaces) {
    setup();
    ASSERT_EQ(console_exec("net join My Home Net  "), 0);
    ASSERT(strcmp(net_ssid(), "My Home Net") == 0);
    ASSERT_EQ(console_exec("net psk pass word 1"), 0);
    ASSERT(net_psk_set());
    ASSERT_EQ(console_exec("net server dw.local 65505"), 0);
    ASSERT(strcmp(net_host(), "dw.local") == 0);
    ASSERT_EQ(net_port(), 65505);
    ASSERT_EQ(console_exec("net server 10.0.0.5"), 0);
    ASSERT_EQ(net_port(), 65504);
    ASSERT_EQ(console_exec("net join"), -1);          /* usage */
    ASSERT_EQ(console_exec("net server"), -1);
    ASSERT_EQ(console_exec("net server x 70000"), -1);  /* bad port */
}

TEST(becker_net_refused_without_config) {
    setup();
    outn = 0;
    ASSERT_EQ(console_exec("becker net"), -1);
    ASSERT(strstr(out, "set ssid and server first") || strstr(out, "needs Plus-W"));
    ASSERT_EQ(mode_get(), MODE_DIAG);
    console_exec("net join a"); console_exec("net server b");
    outn = 0;
    ASSERT_EQ(console_exec("becker net"), -1);          /* host stub: no radio */
    ASSERT(strstr(out, "needs Plus-W"));
    ASSERT_EQ(mode_get(), MODE_DIAG);
    ASSERT_EQ(console_exec("net mode native"), 0);
    ASSERT_EQ(mode_get(), MODE_NATIVE);
}

TEST(net_status_hides_psk_and_save_order) {
    setup();
    console_exec("net join Lab"); console_exec("net psk s3cret"); console_exec("net server 10.0.0.9 65504");
    outn = 0;
    ASSERT_EQ(console_exec("net status"), 0);
    ASSERT(strstr(out, "net state off\n"));
    ASSERT(strstr(out, "net ssid Lab\n"));
    ASSERT(strstr(out, "net psk set\n"));
    ASSERT(!strstr(out, "s3cret"));
    ASSERT(strstr(out, "net server 10.0.0.9 65504\n"));
    outn = 0;
    ASSERT_EQ(console_exec("net"), 0);                  /* alias of status */
    ASSERT(strstr(out, "net state off\n"));
    ASSERT_EQ(console_exec("save"), 0);
    char cfg[1024]; int n = plat_cfg_read(cfg, sizeof cfg - 1); ASSERT(n > 0); cfg[n] = 0;
    const char *j = strstr(cfg, "net join Lab\n"), *p = strstr(cfg, "net psk s3cret\n"),
               *s = strstr(cfg, "net server 10.0.0.9 65504\n"), *b = strstr(cfg, "becker ");
    ASSERT(j && p && s && b);
    ASSERT(j < p && p < s && s < b);
    console_exec("net forget");
    ASSERT(!net_configured());
    console_exec("save");
    n = plat_cfg_read(cfg, sizeof cfg - 1); cfg[n] = 0;
    ASSERT(!strstr(cfg, "net join"));
}

TEST(net_remote_allowed) {
    setup();
    ASSERT_EQ(remote("net status"), 0);
    ASSERT(strstr(rbuf, "net state off\n"));
    ASSERT_EQ(remote("net join Lab"), 0);
    ASSERT_EQ(remote("net psk x"), 0);
    ASSERT_EQ(remote("net server 1.2.3.4"), 0);
    ASSERT_EQ(remote("net mode native"), 0);
    ASSERT_EQ(remote("becker net"), 255);               /* becker stays console-only */
    ASSERT_EQ(remote("net scan"), 255);                 /* stub: refused as "no radio" -> err */
    ASSERT(strcmp(rbuf, "net: needs Plus-W") == 0);
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
    RUN(save_preserves_rw_name_with_spaces);
    RUN(config_bad_line_continues);
    RUN(capture_writes_file);
    RUN(native_pump_end_to_end);
    RUN(native_pump_backpressure);
    RUN(selftest_passes);
    RUN(bus_drive_cmd);
    RUN(disk_show_format);
    RUN(disk_insert_and_eject);
    RUN(disk_insert_name_with_spaces);
    RUN(disk_insert_long_name_fails);
    RUN(disk_insert_replaces_mounted);
    RUN(time_reports_clock_lost_on_host);
    RUN(remote_allowed_commands_run);
    RUN(remote_refuses_console_only);
    RUN(remote_refuses_mounting_config);
    RUN(remote_refuses_config_bypass_backslash);
    RUN(remote_error_codes);
    RUN(remote_output_truncates);
    RUN(remote_tiny_cap_no_crash);
    RUN(remote_via_vserial_end_to_end);
    RUN(fs_new_makes_formatted_rsdos_image);
    RUN(fs_new_refuses_existing);
    RUN(fs_new_refuses_config_name);
    RUN(fs_new_refuses_backslash_config);
    RUN(dw_mount_refuses_backslash_config);
    RUN(fs_new_uses_full_raw_name);
    RUN(fs_new_refuses_long_name);
    RUN(rom_boot_records_without_loading);
    RUN(version_is_1_3);
    RUN(bus_selftest_is_pico_only_on_host);
    RUN(cart_command_and_save);
    RUN(net_join_keeps_spaces);
    RUN(becker_net_refused_without_config);
    RUN(net_status_hides_psk_and_save_order);
    RUN(net_remote_allowed);
    TEST_MAIN_END
}
