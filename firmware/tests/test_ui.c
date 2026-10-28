#include "test.h"
#include "ui.h"
#include "plat.h"
#include "ui_text.h"
#include "net.h"
#include "dw_store.h"
#include "dw.h"
#include "console.h"
#include "mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[300];
static dw_store store;
static dw_server dw;
static uint8_t tx[4096];
static size_t txn;
static char last_line[80];
static char lines[16][80];      /* every console line the Manager ran, in order */
static int nlines;
static int exec_rc;

static void send_cb(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    memcpy(tx + txn, buf, n);
    txn += n;
}
static int exec_cb(const char *line, char *msg, size_t cap) {
    snprintf(last_line, sizeof last_line, "%s", line);
    if (nlines < 16) snprintf(lines[nlines++], sizeof lines[0], "%s", line);
    if (exec_rc) snprintf(msg, cap, "rom load failed");
    return exec_rc;
}

static void mkfile(const char *name, const char *head, size_t size) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "wb");
    uint8_t z[8192];
    memset(z, 0, sizeof z);
    memcpy(z, head, strlen(head));
    for (size_t left = size; left; ) {
        size_t n = left < sizeof z ? left : sizeof z;
        fwrite(z, 1, n, f);
        left -= n;
        memset(z, 0, sizeof z);
    }
    fclose(f);
}

static void wipe(void) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -f '%s'/*", g_dir);
    (void)system(cmd);
}

static void setup(void) {
    wipe();
    txn = 0;
    exec_rc = 0;
    last_line[0] = '\0';
    nlines = 0;
    net_stub_set_available(false);
    net_stub_set_scan(NULL, -1);
    dw_store_posix_init(&store, g_dir);
    dw_init(&dw, &store, NULL, NULL);
    ui_init(send_cb, NULL, exec_cb, &store, &dw);
}

static void out_cb(void *ctx, const char *s);
/* setup, then the console the Manager runs its verbs through (its home screen reads rom next). */
static void setup_console(void) {
    setup();
    console_init(out_cb, NULL, &dw, &store); ui_init(send_cb, NULL, exec_cb, &store, &dw);
}

/* Send one poll the way the stub does. caps < 0: not a first poll. */
static void poll(uint8_t flags, uint8_t key, int caps, uint32_t now) {
    uint8_t p[5];
    size_t n = 0;
    p[n++] = 'P'; p[n++] = flags; p[n++] = key;
    if (caps >= 0) p[n++] = (uint8_t)caps;
    uint8_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    p[n++] = s;
    txn = 0;
    ui_feed(p, n, now);
}

static void open_roms(int capsv);
static void expect_clear_then(uint8_t act);

/* Check the framing of the reply in tx; returns the action-byte count or -1. */
static int reply_ok(void) {
    if (txn < 4) return -1;
    size_t len = ((size_t)tx[0] << 8) | tx[1];
    if (txn != len + 4) return -1;
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) sum += tx[2 + i];
    if ((((unsigned)tx[2 + len] << 8) | tx[3 + len]) != (sum & 0xFFFF)) return -1;
    return (int)len;
}

TEST(inactive_until_ctl) {
    setup();
    ASSERT(!ui_active());
    ui_ctl(0xA5);
    ASSERT(ui_active());
    ui_ctl(0x5A);
    ASSERT(!ui_active());
}

TEST(first_poll_clears_and_draws) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);                       /* 16K, no ECB */
    int len = reply_ok();
    ASSERT(len > 0);
    ASSERT_EQ(tx[2], UI_ACT_CLEAR);
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    char row[UI_COLS + 1];
    ui_row_text(0, row);
    ASSERT(strstr(row, "PICOCO") != NULL);
    ASSERT(strstr(row, "16K") != NULL);
    ASSERT(strstr(row, "NO ECB") != NULL);
}

TEST(caps_shown) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, UI_CAP_32K | UI_CAP_ECB, 0);
    char row[UI_COLS + 1];
    ui_row_text(0, row);
    ASSERT(strstr(row, "32K") != NULL);
    ASSERT(strstr(row, "NO ECB") == NULL);
    ASSERT(strstr(row, "ECB") != NULL);
}

TEST(coco3_header_shows_ram_size) {
    static const struct { int bits; const char *want; } t[] = {
        { 0x00, "PICOCO  COCO 3 128K" }, { 0x40, "PICOCO  COCO 3 512K" },
        { 0x80, "PICOCO  COCO 3 1M" },   { 0xC0, "PICOCO  COCO 3 2M" },
    };
    for (int i = 0; i < 4; i++) {
        setup();
        ui_ctl(0xA5);
        poll(2, 0, UI_CAP_COCO3 | UI_CAP_ECB | UI_CAP_64K | UI_CAP_32K | t[i].bits, 0);
        char row[UI_COLS + 1];
        ui_row_text(0, row);
        ASSERT(strncmp(row, t[i].want, strlen(t[i].want)) == 0);   /* the transport and version follow */
        ASSERT(strstr(row, "64K") == NULL);
        ASSERT(strstr(row, "ECB") == NULL);
    }
}

TEST(second_poll_sends_only_changes) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);
    poll(0, 0, -1, 10);                     /* no key: nothing changed */
    ASSERT_EQ(reply_ok(), 1);               /* just UI_ACT_END */
    ASSERT_EQ(tx[2], UI_ACT_END);
}

TEST(resend_repeats_last_reply) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);
    uint8_t first[sizeof tx];
    size_t n = txn;
    memcpy(first, tx, n);
    poll(3, 0, 0, 10);                      /* first + resend */
    ASSERT_EQ(txn, n);
    ASSERT_MEMEQ(tx, first, n);
}

/* Review Focus 4: bad checksum, half a poll, noise. */
TEST(bad_poll_gets_no_reply) {
    setup();
    ui_ctl(0xA5);
    uint8_t bad[5] = { 'P', 2, 0, 0, 0x99 };
    txn = 0;
    ui_feed(bad, sizeof bad, 0);
    ASSERT_EQ(txn, 0);
    uint8_t noise[3] = { 0x00, 0xFF, 0x41 };
    ui_feed(noise, sizeof noise, 1);
    ASSERT_EQ(txn, 0);
}

TEST(half_poll_recovers_after_quiet) {
    setup();
    ui_ctl(0xA5);
    uint8_t half[2] = { 'P', 2 };
    txn = 0;
    ui_feed(half, sizeof half, 0);
    ASSERT_EQ(txn, 0);
    poll(2, 0, 0, 300);                     /* 300 ms later: parser has reset */
    ASSERT(reply_ok() > 0);
}

TEST(reply_never_exceeds_cap) {
    setup();
    for (int i = 0; i < 12; i++) {
        char n[40];
        snprintf(n, sizeof n, "%02d_abcdefghijklmnopqrstuvwx.rom", i);   /* 31 chars */
        mkfile(n, "", 8192);
    }
    open_roms(0);
    int len = reply_ok();
    ASSERT(len > 0 && len <= UI_REPLY_MAX);
    char row[UI_COLS + 1];
    ui_row_text(13, row);
    ASSERT(row[2] != ' ');                  /* the list really drew */
}

static void open_with(int capsv) {
    ui_ctl(0xA5);
    poll(2, 0, capsv, 0);
}

static bool row_has(int row, const char *s) {
    char t[UI_COLS + 1];
    ui_row_text(row, t);
    return strstr(t, s) != NULL;
}

/* The home screen opens first; ENTER on its first menu row is ROMS. */
static void open_roms(int capsv) {
    open_with(capsv);
    poll(0, UI_KEY_ENTER, -1, 5);
}

TEST(home_screen_shows_drives_and_menu) {
    setup();
    mkfile("game.dsk", "", 256);
    ASSERT_EQ(dw_mount(&dw, 1, "game.dsk", true), 0);
    open_with(UI_CAP_32K | UI_CAP_ECB);
    ASSERT(row_has(0, PICOCO_VERSION));
    ASSERT(row_has(2, "D0 -"));
    ASSERT(row_has(3, "D1 GAME.DSK"));
    ASSERT(row_has(3, "RO"));
    ASSERT(row_has(5, "D3 -"));
    ASSERT(row_has(6, "ROM NEXT"));
    ASSERT(row_has(8, "ROMS"));
    ASSERT(row_has(9, "DISKS"));
    ASSERT(row_has(10, "SETTINGS"));
    ASSERT(row_has(11, "WIFI"));
    ASSERT(row_has(15, "ENTER=OPEN  BREAK=BASIC"));
    poll(0, UI_KEY_ENTER, -1, 5);            /* ROMS */
    ASSERT(row_has(1, "ROMS") || row_has(1, "NO .ROM FILES"));
    ASSERT(!row_has(8, "ROMS"));
    poll(0, UI_KEY_BREAK, -1, 6);            /* back home, still in the session */
    ASSERT(ui_active());
    ASSERT(row_has(15, "ENTER=OPEN  BREAK=BASIC"));
    poll(0, UI_KEY_DOWN, -1, 7);
    poll(0, UI_KEY_ENTER, -1, 8);            /* DISKS */
    ASSERT(row_has(1, "DISKS"));
    ASSERT_EQ(nlines, 0);                    /* browsing changes nothing */
}

/* Review Focus 1 */
TEST(no_roms_message_and_enter_is_harmless) {
    setup();
    open_roms(0);
    ASSERT(row_has(1, "NO .ROM FILES"));
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len > 0);
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    for (int i = 0; i < len; i++) ASSERT(tx[2 + i] != UI_ACT_JUMP || i != len - 2);
    ASSERT(ui_active());
}

TEST(lists_roms_sorted_and_hides_others) {
    setup();
    mkfile("zeta.rom", "\x7E\xC0\x10", 8192);
    mkfile("ALPHA.ROM", "\x7E\xC0\x10", 8192);
    mkfile("disk.dsk", "", 256);
    mkfile("picoco.cfg", "", 16);
    open_roms(0);
    ASSERT(row_has(1, "ROMS"));
    ASSERT(row_has(2, "ALPHA.ROM"));
    ASSERT(row_has(3, "ZETA.ROM"));         /* lowercase folded */
    ASSERT(!row_has(4, "DISK"));
}

/* Review Focus 3 */
TEST(awkward_names_hidden) {
    setup();
    mkfile("my game.rom", "\x7E\xC0\x10", 8192);
    mkfile("abcdefghijklmnopqrstuvwxyz012.rom", "\x7E\xC0\x10", 8192);   /* 33 chars */
    mkfile("ok.rom", "\x7E\xC0\x10", 8192);
    open_roms(0);
    ASSERT(row_has(2, "OK.ROM"));
    ASSERT(!row_has(3, "ROM"));
}

/* Review Focus 2 */
TEST(scrolls_with_the_selection) {
    setup();
    char name[16];
    for (int i = 0; i < 20; i++) {
        snprintf(name, sizeof name, "r%02d.rom", i);
        mkfile(name, "\x7E\xC0\x10", 8192);
    }
    open_roms(0);
    ASSERT(row_has(2, "R00.ROM"));
    ASSERT(row_has(13, "R11.ROM"));
    for (int i = 0; i < 12; i++) poll(0, UI_KEY_DOWN, -1, 10 + (uint32_t)i);
    ASSERT(row_has(13, "R12.ROM"));          /* selection moved past the window: list scrolled */
    ASSERT(row_has(2, "R01.ROM"));
    for (int i = 0; i < 100; i++) poll(0, UI_KEY_DOWN, -1, 100 + (uint32_t)i);
    ASSERT(row_has(13, "R19.ROM"));          /* stops at the last entry */
    for (int i = 0; i < 100; i++) poll(0, UI_KEY_UP, -1, 300 + (uint32_t)i);
    ASSERT(row_has(2, "R00.ROM"));
}

/* Review Focus 2: which 128 of 130 are kept depends on directory order, so
 * only the notice is asserted. */
TEST(more_than_128_shows_truncated) {
    setup();
    char name[16];
    for (int i = 0; i < 130; i++) {
        snprintf(name, sizeof name, "r%03d.rom", i);
        mkfile(name, "\x7E\xC0\x10", 8192);
    }
    open_roms(0);
    ASSERT(row_has(14, "LIST TRUNCATED"));
}

TEST(enter_on_pak_asks_for_jump_then_go_loads) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_roms(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP);
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(txn, 1);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT(strcmp(last_line, "rom launch game.rom") == 0);
    ASSERT(!ui_active());
}

TEST(dos_rom_needs_ecb) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_roms(0);                            /* no ECB */
    poll(0, UI_KEY_ENTER, -1, 10);
    ASSERT(row_has(14, "NEEDS EXTENDED BASIC"));
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    ASSERT(len < 2 || tx[2 + len - 2] != UI_ACT_COLD);
}

TEST(dos_rom_with_ecb_cold_restarts) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_roms(UI_CAP_32K | UI_CAP_ECB);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_COLD);
}

#define CAPS_COCO3 (UI_CAP_64K | UI_CAP_32K | UI_CAP_ECB | UI_CAP_COCO3)

/* Review Focus 1: a CoCo 3 runs the cart from a RAM copy, so JMP $C000 would
 * land in the manager. The pak must be started in ROM mode. */
TEST(coco3_pak_uses_the_rom_mode_jump) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_roms(CAPS_COCO3);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP3);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT(strcmp(last_line, "rom launch game.rom") == 0);
}

TEST(coco3_dos_rom_uses_the_coco3_cold_restart) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_roms(CAPS_COCO3);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_COLD3);
}

/* Review Focus 2 */
TEST(coco3_break_uses_the_coco3_warm_restart) {
    setup();
    open_with(CAPS_COCO3);
    poll(0, UI_KEY_BREAK, -1, 10);
    int len = reply_ok();
    ASSERT(len >= 2);
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_WARM3);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
}

/* A leave action is preceded by a clear and nothing else is drawn. */
static void expect_clear_then(uint8_t act) {
    int len = reply_ok();
    ASSERT_EQ(len, 3);
    ASSERT_EQ(tx[2], UI_ACT_CLEAR);
    ASSERT_EQ(tx[3], act);
    ASSERT_EQ(tx[4], UI_ACT_END);
}

TEST(launch_reply_clears_the_screen) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_roms(UI_CAP_32K | UI_CAP_ECB);
    poll(0, UI_KEY_ENTER, -1, 10);
    expect_clear_then(UI_ACT_JUMP);
}

TEST(break_reply_clears_the_screen) {
    setup();
    open_with(UI_CAP_32K | UI_CAP_ECB);
    poll(0, UI_KEY_BREAK, -1, 10);
    expect_clear_then(UI_ACT_WARM);
}

TEST(coco3_break_reply_clears_the_screen) {
    setup();
    open_with(CAPS_COCO3);
    poll(0, UI_KEY_BREAK, -1, 10);
    expect_clear_then(UI_ACT_WARM3);
}

/* The CoCo 1/2 codes are unchanged. */
TEST(coco2_codes_unchanged) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_roms(UI_CAP_32K | UI_CAP_ECB);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP);
    poll(0, UI_KEY_BREAK, -1, 20);           /* back to the home screen */
    poll(0, UI_KEY_BREAK, -1, 21);           /* and out */
    len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_WARM);
}

/* ADR 0011: every change the Manager makes is saved at once. */
TEST(default_rom_is_set_and_saved) {
    setup_console();
    mkfile("hdb.rom", "DK", 8192);
    open_roms(UI_CAP_32K | UI_CAP_ECB);
    poll(0, 'D', -1, 10);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "rom boot hdb.rom") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
    ASSERT(row_has(14, "NEXT BOOT SET"));
    ASSERT_EQ(console_exec("rom boot hdb.rom"), 0);   /* what the faked line would have done */
    poll(0, 0, -1, 11);
    ASSERT(row_has(2, "* HDB.ROM"));           /* the mark by the next-boot ROM */
    ASSERT(ui_active());
}

TEST(launch_is_not_saved) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_roms(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(nlines, 1);
    ASSERT(strcmp(lines[0], "rom launch game.rom") == 0);
}

/* The Loader (spec 7.2, #60) sends L + sum8 before it jumps into the Stub. */
TEST(loader_request_launches_the_manager) {
    setup();
    ui_ctl(0xA5);
    txn = 0;
    uint8_t bad[2] = { 'L', 'X' };
    ui_feed(bad, 2, 10);
    ASSERT_EQ(txn, 0);                       /* wrong sum: no reply, the Loader times out */
    ASSERT_EQ(nlines, 0);
    uint8_t good[2] = { 'L', 'L' };
    ui_feed(good, 2, 20);
    ASSERT_EQ(txn, 1);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT_EQ(nlines, 1);
    ASSERT(strcmp(lines[0], "rom launch manager") == 0);
    ASSERT(ui_active());
    exec_rc = -1;
    txn = 0;
    ui_feed(good, 2, 30);
    ASSERT_EQ(tx[0], UI_GO_FAIL);
}

/* No previous ROM known (this harness has no console): BREAK on a Loader
 * entry is a plain warm return with nothing to load. */
TEST(loader_entry_break_without_previous_rom) {
    setup();
    open_with(UI_CAP_32K | UI_CAP_ECB | UI_CAP_DOS);
    poll(0, UI_KEY_BREAK, -1, 10);
    expect_clear_then(UI_ACT_WARM);
    uint8_t g = 'G';
    txn = 0;
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT_EQ(nlines, 0);
}

static void open_disks(int capsv) {
    open_with(capsv);
    poll(0, UI_KEY_DOWN, -1, 5);
    poll(0, UI_KEY_ENTER, -1, 6);
}

/* Bench J.5.3: the Loader's disk sits in drive 3; `0` on it must move it, not refuse. */
TEST(mounting_an_image_held_by_another_drive_moves_it) {
    setup();
    mkfile("picoco.dsk", "", 256);
    ASSERT_EQ(dw_mount(&dw, 3, "picoco.dsk", false), 0);
    open_disks(0);
    ASSERT(row_has(2, "3 PICOCO.DSK"));
    nlines = 0;
    poll(0, '0', -1, 10);
    ASSERT_EQ(nlines, 4);
    ASSERT(strcmp(lines[0], "dw disk eject 3") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
    ASSERT(strcmp(lines[2], "dw disk insert 0 picoco.dsk") == 0);
    ASSERT(strcmp(lines[3], "save") == 0);
}

TEST(disks_lists_images_not_roms_or_config) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    mkfile("picoco.cfg", "", 16);
    mkfile("my disk.dsk", "", 256);
    mkfile("ALPHA.DSK", "", 256);
    open_disks(0);
    ASSERT(row_has(1, "DISKS"));
    ASSERT(row_has(2, "ALPHA.DSK"));
    ASSERT(row_has(3, "MY DISK.DSK"));      /* spaces are fine: dw disk insert takes the raw tail */
    ASSERT(!row_has(4, "."));
}

TEST(mount_and_eject_are_saved_and_shown) {
    setup();
    mkfile("alpha.dsk", "", 256);
    mkfile("beta.dsk", "", 256);
    open_disks(0);
    exec_rc = 0;
    poll(0, UI_KEY_DOWN, -1, 10);
    poll(0, '2', -1, 11);                    /* mount beta in drive 2 */
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "dw disk insert 2 beta.dsk") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
    ASSERT_EQ(dw_mount(&dw, 2, "beta.dsk", false), 0);   /* what the console line would have done */
    poll(0, 0, -1, 12);
    ASSERT(row_has(3, "2 BETA.DSK") || row_has(3, "2BETA.DSK"));
    poll(0, 'E', -1, 13);
    ASSERT(row_has(14, "EJECT"));
    poll(0, '2', -1, 14);
    ASSERT_EQ(nlines, 4);
    ASSERT(strcmp(lines[2], "dw disk eject 2") == 0);
    ASSERT(strcmp(lines[3], "save") == 0);
    poll(0, UI_KEY_BREAK, -1, 15);
    ASSERT(row_has(4, "D2 BETA.DSK"));      /* home shows the drive table as it is */
}

TEST(failed_change_shows_the_reason_and_does_not_save) {
    setup();
    mkfile("alpha.dsk", "", 256);
    open_disks(0);
    exec_rc = -1;
    poll(0, '0', -1, 10);
    ASSERT_EQ(nlines, 1);
    ASSERT(row_has(14, "ROM LOAD FAILED"));  /* the test exec's reason text */
}

/* ---- Boot screen (#66) ---- */

#define DSK_SIZE (35u * 18u * 256u)
static uint8_t img[DSK_SIZE];
static void out_cb(void *ctx, const char *s) { (void)ctx; (void)s; }

/* One RS-DOS directory entry at index i: 8+3 name, type, ASCII flag. */
static void dirent(int i, const char *name, const char *ext, uint8_t type) {
    uint8_t *d = img + 308u * 256u + (uint32_t)i * 32u;
    memset(d, ' ', 11);
    memcpy(d, name, strlen(name));
    memcpy(d + 8, ext, strlen(ext));
    d[11] = type;
}

static void dsk_write(const char *name) {
    char p[600];
    snprintf(p, sizeof p, "%s/%s", g_dir, name);
    FILE *f = fopen(p, "wb");
    fwrite(img, 1, sizeof img, f);
    fclose(f);
}

/* A 35-track image in the store; nent directory entries, then the $FF end marker unless full. */
static void mkdsk(const char *name, bool os9, int nent) {
    memset(img, 0, sizeof img);
    for (int i = 0; i < nent; i++) {
        char n[9];
        snprintf(n, sizeof n, "F%07d", i);
        dirent(i, n, "BAS", 0);
    }
    if (nent < 72) img[308u * 256u + (uint32_t)nent * 32u] = 0xFF;
    if (os9) memcpy(img + 612u * 256u, "OS", 2);
    dsk_write(name);
}

/* Disks screen with game.dsk selected and mounted in drive 0 (the console
 * line is faked here, so the mount is done as that line would). */
static void open_boot(int capsv) {
    open_disks(capsv);
    ASSERT_EQ(dw_mount(&dw, 0, "game.dsk", false), 0);
    nlines = 0;
    poll(0, 'B', -1, 11);
}

TEST(boot_lists_programs_from_the_directory) {
    setup();
    mkdsk("game.dsk", false, 0);
    dirent(0, "GAME", "BAS", 0);
    dirent(1, "DATA", "DAT", 1);          /* data: hidden */
    dirent(2, "DINORUN", "BIN", 2);
    dirent(3, "OLD", "BAS", 0);
    img[308u * 256u + 3 * 32] = 0x00;     /* deleted */
    dirent(4, "NOTES", "TXT", 3);         /* text: hidden */
    img[308u * 256u + 5 * 32] = 0xFF;
    dsk_write("game.dsk");
    open_boot(UI_CAP_64K | UI_CAP_ECB);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "dw disk insert 0 game.dsk") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
    ASSERT(row_has(1, "RUN WHICH"));
    ASSERT(row_has(2, "GAME.BAS"));
    ASSERT(row_has(3, "DINORUN.BIN"));
    ASSERT(!row_has(4, "."));
    ASSERT(!row_has(4, "RUN WHICH"));
    ASSERT(row_has(15, "ENTER=RUN"));
}

TEST(boot_full_directory_has_no_end_marker) {
    setup();
    mkdsk("game.dsk", false, 72);
    open_boot(UI_CAP_64K | UI_CAP_ECB);
    ASSERT(row_has(1, "1-12 OF 72"));
    poll(0, UI_KEY_PGDN, -1, 12);
    poll(0, UI_KEY_PGDN, -1, 13);
    poll(0, UI_KEY_PGDN, -1, 14);
    poll(0, UI_KEY_PGDN, -1, 15);
    poll(0, UI_KEY_PGDN, -1, 16);
    poll(0, UI_KEY_PGDN, -1, 17);
    ASSERT(row_has(13, "F0000071.BAS"));
}

TEST(boot_with_no_programs_stays_on_disks) {
    setup();
    mkdsk("game.dsk", false, 0);
    open_boot(UI_CAP_64K | UI_CAP_ECB);
    ASSERT(row_has(1, "DISKS"));
    ASSERT(row_has(14, "NO BAS/BIN FILES"));
    ASSERT_EQ(nlines, 2);                 /* the mount stays */
}

TEST(boot_refused_in_bridge_and_net_mode) {
    setup();
    mkdsk("game.dsk", true, 0);
    open_disks(UI_CAP_64K | UI_CAP_ECB);
    mode_set(MODE_NET);
    poll(0, 'B', -1, 11);
    ASSERT(row_has(14, "BOOT NEEDS NATIVE MODE"));
    ASSERT_EQ(nlines, 0);
    mode_set(MODE_BRIDGE);
    poll(0, 'B', -1, 12);
    ASSERT(row_has(14, "BOOT NEEDS NATIVE MODE"));
    ASSERT_EQ(nlines, 0);
    mode_reset();
}

TEST(boot_os9_disk_loads_the_boot_track) {
    setup();
    mkdsk("game.dsk", true, 0);
    open_boot(UI_CAP_64K | UI_CAP_ECB);
    int len = reply_ok();
    static const uint8_t want[] = { UI_ACT_CLEAR, UI_ACT_BOOT, 0, 0x00, 0x02, 0x64, 18, 0x26, 0x00, 0x26, 0x02, UI_ACT_END };
    ASSERT_EQ(len, (int)sizeof want);
    ASSERT_MEMEQ(tx + 2, want, sizeof want);
    uint8_t g = 'G';
    txn = 0;
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT_EQ(nlines, 2);                 /* no ROM swap for a NitrOS-9 boot */
    ASSERT(!ui_active());
}

TEST(boot_os9_disk_needs_64k) {
    setup();
    mkdsk("game.dsk", true, 0);
    open_boot(UI_CAP_32K | UI_CAP_ECB);
    ASSERT(row_has(14, "NEEDS 64K"));
    ASSERT(row_has(1, "DISKS"));
}

TEST(boot_cannot_read_an_unmounted_drive) {
    setup();
    mkdsk("game.dsk", true, 0);
    open_disks(UI_CAP_64K | UI_CAP_ECB);
    poll(0, 'B', -1, 11);                 /* the faked insert mounted nothing */
    ASSERT(row_has(14, "CANNOT READ DISK"));
}

TEST(boot_on_a_loader_entry_types_the_line) {
    setup();
    mkdsk("game.dsk", false, 0);
    dirent(0, "GAME", "BAS", 0);
    dirent(1, "DINORUN", "BIN", 2);
    img[308u * 256u + 2 * 32] = 0xFF;
    dsk_write("game.dsk");
    open_boot(UI_CAP_64K | UI_CAP_ECB | UI_CAP_DOS);
    poll(0, UI_KEY_DOWN, -1, 12);
    poll(0, UI_KEY_ENTER, -1, 13);
    static const char text[] = "DRIVE0:LOADM\"DINORUN.BIN\":EXEC\r";
    int len = reply_ok();
    ASSERT_EQ(len, 4 + (int)strlen(text));
    ASSERT_EQ(tx[2], UI_ACT_CLEAR);
    ASSERT_EQ(tx[3], UI_ACT_TYPE);
    ASSERT_EQ(tx[4], strlen(text));
    ASSERT_MEMEQ(tx + 5, text, strlen(text));
    ASSERT_EQ(tx[5 + strlen(text)], UI_ACT_END);
    poll(0, UI_KEY_UP, -1, 14);           /* a key cancels, as for a launch */
    poll(0, UI_KEY_ENTER, -1, 15);
    static const char bas[] = "DRIVE0:RUN\"GAME.BAS\"\r";
    len = reply_ok();
    ASSERT_EQ(tx[3], UI_ACT_TYPE);
    ASSERT_EQ(tx[4], strlen(bas));
    ASSERT_MEMEQ(tx + 5, bas, strlen(bas));
}

TEST(boot_on_a_power_on_entry_offers_the_dos) {
    setup_console();
    mkfile("hdbdos.rom", "DK", 8192);
    mkdsk("game.dsk", false, 1);
    open_boot(UI_CAP_64K | UI_CAP_ECB);
    poll(0, UI_KEY_ENTER, -1, 12);
    ASSERT(row_has(14, "SET A DOS ROM AS DEFAULT"));   /* next boot is the manager */
    ASSERT_EQ(console_exec("rom boot hdbdos.rom"), 0);
    poll(0, UI_KEY_ENTER, -1, 13);
    ASSERT(row_has(13, "STARTING DOS, THEN TYPE:"));
    ASSERT(row_has(14, "RUN\"F0000000.BAS\""));
    nlines = 0;
    poll(0, ' ', -1, 14);
    expect_clear_then(UI_ACT_DOS);
    uint8_t g = 'G';
    txn = 0;
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT_EQ(nlines, 1);
    ASSERT(strcmp(lines[0], "rom launch hdbdos.rom") == 0);
}

TEST(boot_on_a_coco3_power_on_entry_cold_restarts) {
    setup_console();
    mkfile("hdbdos.rom", "DK", 8192);
    ASSERT_EQ(console_exec("rom boot hdbdos.rom"), 0);
    mkdsk("game.dsk", false, 1);
    open_boot(UI_CAP_64K | UI_CAP_ECB | UI_CAP_COCO3);
    poll(0, UI_KEY_ENTER, -1, 12);
    poll(0, UI_KEY_ENTER, -1, 13);
    expect_clear_then(UI_ACT_COLD3);
}

TEST(boot_program_needs_extended_basic) {
    setup();
    mkdsk("game.dsk", false, 1);
    open_boot(UI_CAP_64K);
    poll(0, UI_KEY_ENTER, -1, 12);
    ASSERT(row_has(14, "NEEDS EXTENDED BASIC"));
    ASSERT(tx[2] != UI_ACT_CLEAR);           /* no leave */
}

/* The Loader's L recorded the DOS it replaced; ENTER on a program puts it back on G. */
TEST(boot_on_a_loader_entry_swaps_the_dos_back) {
    setup_console();
    mkfile("hdbdos.rom", "DK", 8192);
    ASSERT_EQ(console_exec("rom load hdbdos.rom"), 0);
    mkdsk("game.dsk", false, 1);
    ui_ctl(0xA5);
    uint8_t load[2] = { 'L', 'L' };
    ui_feed(load, 2, 1);
    ASSERT(strcmp(ui_prev_rom(), "hdbdos.rom") == 0);
    poll(2, 0, UI_CAP_64K | UI_CAP_ECB | UI_CAP_DOS, 2);
    poll(0, UI_KEY_DOWN, -1, 5);
    poll(0, UI_KEY_ENTER, -1, 6);
    ASSERT_EQ(dw_mount(&dw, 0, "game.dsk", false), 0);
    poll(0, 'B', -1, 11);
    poll(0, UI_KEY_ENTER, -1, 12);
    ASSERT_EQ(tx[3], UI_ACT_TYPE);
    nlines = 0;
    uint8_t g = 'G';
    txn = 0;
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT_EQ(nlines, 1);
    ASSERT(strcmp(lines[0], "rom launch hdbdos.rom") == 0);
    ASSERT(ui_prev_rom()[0] == '\0');
    ASSERT(!ui_active());
}

/* An OS-9 RBF or blank image is not refused by the OS check, but its sectors
 * 308-316 are not a directory: no garbage names, the Disks screen stays. */
TEST(boot_ignores_a_non_rsdos_directory) {
    setup();
    mkdsk("game.dsk", false, 0);
    memset(img + 308u * 256u, 0xC3, 9u * 256u);
    for (int e = 0; e < 72; e++) img[308u * 256u + (uint32_t)e * 32u + 11] = 0;   /* "BASIC" type, binary name */
    dsk_write("game.dsk");
    open_boot(UI_CAP_64K | UI_CAP_ECB);
    ASSERT(row_has(1, "DISKS"));
    ASSERT(row_has(14, "NO BAS/BIN FILES"));
}

/* The Loader's recorded DOS is dropped once any leave ends the session. */
TEST(loader_entry_prev_is_dropped_on_a_jump_leave) {
    setup_console();
    mkfile("hdbdos.rom", "DK", 8192);
    ASSERT_EQ(console_exec("rom load hdbdos.rom"), 0);
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    ui_ctl(0xA5);
    uint8_t load[2] = { 'L', 'L' };
    ui_feed(load, 2, 1);
    ASSERT(strcmp(ui_prev_rom(), "hdbdos.rom") == 0);
    poll(2, 0, UI_CAP_64K | UI_CAP_ECB | UI_CAP_DOS, 2);
    poll(0, UI_KEY_ENTER, -1, 5);          /* ROMS: game.rom first */
    poll(0, UI_KEY_ENTER, -1, 6);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT(ui_prev_rom()[0] == '\0');
}

TEST(boot_break_returns_to_disks_with_drive_0_mounted) {
    setup();
    mkdsk("game.dsk", false, 1);
    mkdsk("alpha.dsk", false, 0);
    open_disks(UI_CAP_64K | UI_CAP_ECB);
    poll(0, UI_KEY_DOWN, -1, 10);          /* game.dsk is second */
    ASSERT_EQ(dw_mount(&dw, 0, "game.dsk", false), 0);
    nlines = 0;
    poll(0, 'B', -1, 11);
    ASSERT(row_has(1, "RUN WHICH"));
    poll(0, UI_KEY_BREAK, -1, 12);
    ASSERT(row_has(1, "DISKS"));
    ASSERT(row_has(3, "0 GAME.DSK"));
    ASSERT_EQ(ui_cell(3, 2), 'G' & 0x3F);  /* game.dsk still selected (inverse) */
    ASSERT_EQ(nlines, 2);                  /* insert and save only; no eject */
}

static void open_settings(int capsv);
static uint32_t tnow = 100;
static void type(const char *s) { for (; *s; s++) poll(0, (uint8_t)*s, -1, tnow++); }

TEST(new_disk_keeps_case_and_adds_dsk) {
    setup();
    open_disks(0);
    poll(0, 'N', -1, 10);
    ASSERT(row_has(12, "NAME:"));
    poll(0, 0x01, -1, 11);                   /* a control code below $20 is ignored */
    poll(0, 0x08, -1, 12);                   /* backspace at empty is harmless */
    type("MyDisk1");
    ASSERT_EQ(ui_cell(13, 0), 0x4D);         /* M normal */
    ASSERT_EQ(ui_cell(13, 1), 0x19);         /* y inverse Y */
    ASSERT_EQ(ui_cell(13, 2), 0x44);         /* D normal */
    ASSERT_EQ(ui_cell(13, 3), 0x09);         /* i inverse I */
    ASSERT_EQ(ui_cell(13, 6), 0x71);         /* 1 normal */
    ASSERT_EQ(ui_cell(13, 7), 0x20);         /* cursor: inverse space */
    ASSERT_EQ(ui_cell(13, 8), 0x60);
    poll(0, UI_KEY_ENTER, -1, 200);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "fs new MyDisk1.DSK") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
    ASSERT(row_has(15, "N=NEW"));   /* editor closed, footer back */
}

TEST(new_disk_with_an_extension_is_left_alone) {
    setup();
    open_disks(0);
    poll(0, 'n', -1, 10);
    type("a.img");
    poll(0, UI_KEY_ENTER, -1, 200);
    ASSERT(strcmp(lines[0], "fs new a.img") == 0);
}

TEST(editor_limit_and_cancel) {
    setup();
    open_disks(0);
    poll(0, 'N', -1, 10);
    type("abcdefghijklmnopqrstuvwxyz012345XYZ");   /* 35 keys, 31 fit */
    poll(0, 0x08, -1, 300);
    poll(0, 'Q', -1, 301);                         /* back to 31 */
    poll(0, 'R', -1, 302);                         /* ignored */
    ASSERT_EQ(ui_cell(13, 30), 0x51);              /* Q, normal: the 31st cell */
    ASSERT_EQ(ui_cell(13, 31), 0x20);              /* the cursor follows it */
    poll(0, UI_KEY_BREAK, -1, 303);
    ASSERT_EQ(nlines, 0);
    ASSERT(!row_has(12, "NAME:"));
    ASSERT(row_has(15, "N=NEW"));
    ASSERT(ui_active());
}

TEST(new_disk_name_must_fit_the_list) {
    setup();
    open_disks(0);
    poll(0, 'N', -1, 10);
    type("abcdefghijklmnopqrstuvwxyzab");    /* 28 + .DSK = 32 */
    poll(0, UI_KEY_ENTER, -1, 300);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(14, "NAME TOO LONG"));
    poll(0, 'N', -1, 310);
    type("abcdefghijklmnopqrstuvwxyza");     /* 27 + .DSK = 31 */
    poll(0, UI_KEY_ENTER, -1, 600);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "fs new abcdefghijklmnopqrstuvwxyza.DSK") == 0);
}

static void open_wifi(void) {
    net_stub_set_available(true);
    open_with(0);
    for (int i = 0; i < 3; i++) poll(0, UI_KEY_DOWN, -1, 5 + (uint32_t)i);
    poll(0, UI_KEY_ENTER, -1, 9);
}

TEST(wifi_psk_is_masked_and_saved) {
    setup();
    open_wifi();
    poll(0, 'P', -1, 10);
    type("pW");
    ASSERT_EQ(ui_cell(13, 0), 0x6A);                 /* * normal */
    ASSERT_EQ(ui_cell(13, 1), 0x6A);
    ASSERT_EQ(ui_cell(13, 2), 0x20);
    poll(0, UI_KEY_ENTER, -1, 300);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "net psk pW") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
}

TEST(wifi_scan_pick_and_join_saves_once) {
    static const char *const found[] = { "Home", "Cafe", "Home" };
    setup();
    open_wifi();
    net_stub_set_scan(found, 3);
    poll(0, 'S', -1, 10);
    ASSERT(row_has(2, "HOME"));
    ASSERT(row_has(3, "CAFE"));
    ASSERT(!row_has(4, "HOME"));                     /* one row per network */
    poll(0, UI_KEY_ENTER, -1, 11);
    ASSERT(row_has(12, "PSK:"));
    type("sEcret");
    poll(0, UI_KEY_ENTER, -1, 300);
    ASSERT_EQ(nlines, 3);
    ASSERT(strcmp(lines[0], "net join Home") == 0);
    ASSERT(strcmp(lines[1], "net psk sEcret") == 0);
    ASSERT(strcmp(lines[2], "save") == 0);
}

TEST(wifi_scan_failure_is_shown) {
    setup();
    open_wifi();
    poll(0, 'S', -1, 10);
    ASSERT(row_has(14, "SCAN FAILED"));
    net_stub_set_scan(NULL, 0);
    poll(0, 'S', -1, 11);
    ASSERT(row_has(14, "NO NETWORKS FOUND"));
    ASSERT_EQ(nlines, 0);
}

TEST(wifi_server_host_then_port) {
    setup();
    open_wifi();
    poll(0, 'H', -1, 10);
    type("my.host");
    poll(0, UI_KEY_ENTER, -1, 300);
    ASSERT(row_has(12, "PORT (65504):"));
    poll(0, UI_KEY_ENTER, -1, 301);                  /* empty: the default */
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "net server my.host 65504") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
}

TEST(wifi_server_bad_port_runs_nothing) {
    setup();
    open_wifi();
    poll(0, 'H', -1, 10);
    type("my.host");
    poll(0, UI_KEY_ENTER, -1, 300);
    type("abc");
    poll(0, UI_KEY_ENTER, -1, 600);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(14, "BAD PORT"));
}

TEST(clock_is_set_from_two_prompts) {
    setup();
    open_settings(0);
    poll(0, 'T', -1, 10);
    ASSERT(row_has(12, "DATE YYYY-MM-DD:"));
    ASSERT(row_has(13, "20"));                       /* starts from the current clock */
    for (int i = 0; i < 10; i++) poll(0, 0x08, -1, 20 + (uint32_t)i);
    type("2026-10-15");
    poll(0, UI_KEY_ENTER, -1, 100);
    ASSERT_EQ(nlines, 0);                            /* nothing written until the time is in */
    ASSERT(row_has(12, "TIME HH:MM 24H:"));
    for (int i = 0; i < 5; i++) poll(0, 0x08, -1, 120 + (uint32_t)i);
    type("19:30");
    poll(0, UI_KEY_ENTER, -1, 300);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "time set 1792092600") == 0);   /* date -u -j -f "%Y-%m-%d %H:%M:%S" "2026-10-15 19:30:00" +%s */
    ASSERT(strcmp(lines[1], "save") == 0);
}

TEST(clock_break_on_either_prompt_writes_nothing) {
    setup();
    open_settings(0);
    poll(0, 'T', -1, 10);
    poll(0, UI_KEY_BREAK, -1, 20);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(4, "T:CLOCK 20"));
    poll(0, 'T', -1, 30);
    poll(0, UI_KEY_ENTER, -1, 100);
    ASSERT(row_has(12, "TIME HH:MM 24H:"));
    poll(0, UI_KEY_BREAK, -1, 200);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(4, "T:CLOCK 20"));
}

TEST(bad_clock_text_runs_nothing) {
    setup();
    open_settings(0);
    poll(0, 'T', -1, 10);
    for (int i = 0; i < 10; i++) poll(0, 0x08, -1, 20 + (uint32_t)i);
    type("2026-02-30");
    poll(0, UI_KEY_ENTER, -1, 300);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(14, "BAD DATE/TIME"));            /* refused before the time prompt */
    setup();
    open_settings(0);
    poll(0, 'T', -1, 10);
    poll(0, UI_KEY_ENTER, -1, 100);
    for (int i = 0; i < 5; i++) poll(0, 0x08, -1, 120 + (uint32_t)i);
    type("24:00");
    poll(0, UI_KEY_ENTER, -1, 300);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(14, "BAD DATE/TIME"));
}

/* Z opens the picker on OFF (the test clock has no offset); CUSTOM... is the row below. */
static void open_custom_tz(void) {
    poll(0, 'Z', -1, 10);
    poll(0, UI_KEY_DOWN, -1, 11);
    poll(0, UI_KEY_ENTER, -1, 12);
}

static void tz(const char *text) {
    setup();
    open_settings(0);
    open_custom_tz();
    for (int i = 0; i < 6; i++) poll(0, 0x08, -1, 20 + (uint32_t)i);
    type(text);
    poll(0, UI_KEY_ENTER, -1, 300);
}

TEST(timezone_is_parsed_in_minutes) {
    tz("-05:00"); ASSERT(strcmp(lines[0], "net tz -300") == 0); ASSERT(strcmp(lines[1], "save") == 0);
    tz("OFF");    ASSERT(strcmp(lines[0], "net tz off") == 0);
    tz("+5:30");  ASSERT(strcmp(lines[0], "net tz 330") == 0);
    tz("+14");    ASSERT(strcmp(lines[0], "net tz 840") == 0);
    tz("+14:00"); ASSERT(strcmp(lines[0], "net tz 840") == 0);
    tz("+15");    ASSERT_EQ(nlines, 0); ASSERT(row_has(14, "BAD OFFSET"));   /* net_set_tz allows +-840 */
    tz("x");      ASSERT_EQ(nlines, 0); ASSERT(row_has(14, "BAD OFFSET"));
}

TEST(timezone_picker_enter_sets_and_saves) {
    setup();
    open_settings(0);
    poll(0, 'Z', -1, 10);
    ASSERT(row_has(1, "TIMEZONE"));
    ASSERT(row_has(12, "OFF"));                      /* preselected, on the last page */
    ASSERT(row_has(13, "CUSTOM..."));
    ASSERT(row_has(15, "ENTER=PICK  BREAK=BACK"));
    ASSERT_EQ(nlines, 0);
    for (int i = 0; i < 13; i++) poll(0, UI_KEY_UP, -1, 20 + (uint32_t)i);   /* OFF up to EST */
    ASSERT(row_has(2, "UTC-05:00 EST / CDT"));
    poll(0, UI_KEY_ENTER, -1, 100);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "net tz -300") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
    ASSERT(row_has(1, "SETTINGS"));
}

TEST(timezone_picker_preselects_the_current_offset) {
    setup();
    open_settings(0);
    poll(0, 'Z', -1, 10);
    poll(0, UI_KEY_ENTER, -1, 20);                   /* the preselected row is OFF */
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "net tz off") == 0);
    setup();
    open_settings(0);
    poll(0, 'Z', -1, 10);
    poll(0, UI_KEY_BREAK, -1, 20);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(1, "SETTINGS"));
}

TEST(timezone_custom_falls_through_to_the_prompt) {
    setup();
    open_settings(0);
    open_custom_tz();
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(12, "UTC OFFSET (+HH:MM, -HH, OFF):"));
    ASSERT(row_has(13, "OFF"));                      /* prefilled with the current offset */
    poll(0, UI_KEY_BREAK, -1, 20);
    ASSERT_EQ(nlines, 0);
    ASSERT(row_has(1, "SETTINGS"));
}

TEST(settings_names_the_zone_of_the_offset) {
    setup_console();
    ASSERT_EQ(console_exec("net tz -300"), 0);
    open_settings(0);
    ASSERT(row_has(5, "Z:TIMEZONE UTC-05:00 EST / CDT"));
    poll(0, 'Z', -1, 10);
    poll(0, UI_KEY_ENTER, -1, 20);                   /* preselected on the current offset */
    ASSERT(strcmp(last_line, "save") == 0);
    ASSERT_EQ(net_tz(), -300);
    poll(0, 'Z', -1, 30);
    for (int i = 0; i < 14; i++) poll(0, UI_KEY_DOWN, -1, 40 + (uint32_t)i);   /* EST down to CUSTOM... */
    poll(0, UI_KEY_ENTER, -1, 60);
    ASSERT(row_has(13, "-05:00"));
    ASSERT_EQ(console_exec("net tz 120"), 0);
    poll(0, UI_KEY_BREAK, -1, 65);
    ASSERT(row_has(5, "Z:TIMEZONE UTC+02:00 EET / CEST"));   /* the longest row text, whole */
    ASSERT_EQ(console_exec("net tz 210"), 0);        /* no row names +03:30 */
    poll(0, 0, -1, 70);                              /* a redraw */
    ASSERT(row_has(5, "Z:TIMEZONE UTC+03:30"));
    poll(0, 'Z', -1, 80);
    poll(0, UI_KEY_ENTER, -1, 90);
    ASSERT(row_has(12, "UTC OFFSET"));               /* an unlisted offset preselects CUSTOM... */
    ASSERT_EQ(console_exec("net tz off"), 0);        /* the net stub outlives this test */
}

TEST(list_pages_counts_and_jumps) {
    setup();
    char name[16];
    for (int i = 0; i < 30; i++) {
        snprintf(name, sizeof name, "%c%02d.rom", 'A' + i / 10, i);
        mkfile(name, "\x7E\xC0\x10", 8192);
    }
    open_roms(0);
    ASSERT(row_has(1, "1-12 OF 30"));
    poll(0, UI_KEY_PGDN, -1, 10);
    ASSERT(row_has(13, "B12.ROM"));         /* selection 12 is the last visible row */
    ASSERT(row_has(1, "2-13 OF 30"));
    poll(0, UI_KEY_PGDN, -1, 11);
    poll(0, UI_KEY_PGDN, -1, 12);
    ASSERT(row_has(13, "C29.ROM"));         /* clamps at the end */
    poll(0, UI_KEY_PGUP, -1, 13);
    poll(0, UI_KEY_PGUP, -1, 14);
    poll(0, UI_KEY_PGUP, -1, 15);
    ASSERT(row_has(2, "A00.ROM"));
    poll(0, 'G', -1, 20);
    poll(0, 'c', -1, 21);                   /* lowercase mode: still a jump to C */
    ASSERT(row_has(13, "C20.ROM"));         /* the window scrolls just enough, as for any move */
    ASSERT(row_has(1, "10-21 OF 30"));
    poll(0, 'G', -1, 22);
    poll(0, '2', -1, 23);                   /* within a second: prefix C2 */
    ASSERT(row_has(13, "C20.ROM"));
    poll(0, 'G', -1, 2000);
    poll(0, 'B', -1, 2001);                 /* a second later: a new prefix */
    ASSERT(row_has(3, "B10.ROM"));
    ASSERT_EQ(nlines, 0);
}

/* The arrow codes are printable in the stub's key map; after G they still move. */
TEST(g_then_an_arrow_key_moves) {
    setup();
    char name[16];
    for (int i = 0; i < 30; i++) {
        snprintf(name, sizeof name, "%c%02d.rom", 'A' + i / 10, i);
        mkfile(name, "\x7E\xC0\x10", 8192);
    }
    open_roms(0);
    poll(0, 'G', -1, 10);
    poll(0, UI_KEY_PGDN, -1, 11);
    ASSERT(row_has(1, "2-13 OF 30"));
    poll(0, 'G', -1, 12);
    poll(0, UI_KEY_PGUP, -1, 13);
    ASSERT(row_has(1, "1-12 OF 30"));
    poll(0, UI_KEY_DOWN, -1, 14);
    poll(0, 'G', -1, 15);
    poll(0, UI_KEY_UP, -1, 16);
    ASSERT_EQ(ui_cell(2, 2), 'A' & 0x3F);    /* A00.ROM selected again (inverse) */
}

TEST(g_then_a_control_key_still_acts) {
    setup();
    mkfile("a.rom", "\x7E\xC0\x10", 8192);
    open_roms(0);
    poll(0, 'G', -1, 10);
    poll(0, UI_KEY_BREAK, -1, 11);
    ASSERT(row_has(15, "ENTER=OPEN  BREAK=BASIC"));   /* back home, not swallowed as a jump letter */
}

TEST(eject_arm_lasts_one_key) {
    setup();
    mkfile("alpha.dsk", "", 256);
    mkfile("beta.dsk", "", 256);
    open_disks(0);
    poll(0, 'E', -1, 10);
    poll(0, UI_KEY_DOWN, -1, 11);           /* changed my mind, picked another image */
    poll(0, '1', -1, 12);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "dw disk insert 1 beta.dsk") == 0);
}

TEST(new_session_starts_without_last_sessions_message) {
    setup();
    mkfile("alpha.dsk", "", 256);
    open_disks(0);
    exec_rc = -1;
    poll(0, '0', -1, 10);
    ASSERT(row_has(14, "ROM LOAD FAILED"));
    ui_ctl(0xA5);
    poll(2, 0, 0, 20);
    ASSERT(!row_has(14, "FAILED"));
}

TEST(disks_lists_image_files_only) {
    setup();
    mkfile("firmware.uf2", "", 256);
    mkfile("notes.txt", "", 16);
    mkfile("a.dsk", "", 256);
    mkfile("b.jvc", "", 256);
    mkfile("c.vdk", "", 256);
    open_disks(0);
    ASSERT(row_has(2, "A.DSK"));
    ASSERT(row_has(3, "B.JVC"));
    ASSERT(row_has(4, "C.VDK"));
    ASSERT(!row_has(5, "."));
}

TEST(home_long_read_only_name_keeps_ro) {
    setup();
    mkfile("abcdefghijklmnopqrstuvwxyz0.dsk", "", 256);   /* 31 chars */
    ASSERT_EQ(dw_mount(&dw, 1, "abcdefghijklmnopqrstuvwxyz0.dsk", true), 0);
    open_with(0);
    char row[UI_COLS + 1];
    ui_row_text(3, row);
    ASSERT(strncmp(row, "D1 ABCDEFGHIJKLMNOPQRSTUVW", 26) == 0);
    ASSERT(strcmp(row + UI_COLS - 2, "RO") == 0);
}

static void open_settings(int capsv) {
    open_with(capsv);
    poll(0, UI_KEY_DOWN, -1, 5);
    poll(0, UI_KEY_DOWN, -1, 6);
    poll(0, UI_KEY_ENTER, -1, 7);
}

TEST(settings_toggle_hdbdos_is_saved) {
    setup();
    open_settings(0);
    ASSERT(row_has(1, "SETTINGS"));
    ASSERT(row_has(3, "HDB-DOS DRIVE MODE ON"));
    ASSERT(row_has(4, "T:CLOCK 20"));       /* a formatted date, not a raw number */
    ASSERT(row_has(5, "Z:TIMEZONE OFF"));
    poll(0, 'H', -1, 10);
    ASSERT_EQ(nlines, 2);
    ASSERT(strcmp(lines[0], "dw hdbdos off") == 0);
    ASSERT(strcmp(lines[1], "save") == 0);
}

TEST(wifi_screen_without_a_radio_is_one_line) {
    setup();
    open_with(0);
    for (int i = 0; i < 3; i++) poll(0, UI_KEY_DOWN, -1, 5 + (uint32_t)i);
    poll(0, UI_KEY_ENTER, -1, 9);
    ASSERT(row_has(1, "WIFI"));
    ASSERT(row_has(3, "NO RADIO ON THIS BOARD"));
    poll(0, 'M', -1, 10);
    ASSERT_EQ(nlines, 0);
    poll(0, UI_KEY_BREAK, -1, 11);
    ASSERT(row_has(11, "WIFI"));
    ASSERT(row_has(11, "NO RADIO"));
}

TEST(bad_size_refused_before_leaving) {
    setup();
    mkfile("odd.rom", "\x7E\xC0\x10", 5000);
    open_roms(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    ASSERT(row_has(14, "NOT A ROM SIZE"));
    ASSERT(ui_active());
}

/* Review Focus 5 */
TEST(go_failure_keeps_session_and_shows_reason) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_roms(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    exec_rc = -1;
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_FAIL);
    ASSERT(ui_active());
    ui_ctl(0xA5);                            /* the stub starts over */
    poll(2, 0, 0, 30);
    ASSERT(row_has(14, "ROM LOAD FAILED"));
}

TEST(go_without_a_pending_launch_fails) {
    setup();
    open_with(0);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_FAIL);
}

TEST(break_asks_for_warm_restart) {
    setup();
    open_with(0);
    poll(0, UI_KEY_BREAK, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_WARM);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_OK);
    ASSERT(last_line[0] == '\0');            /* nothing to load */
    ASSERT(!ui_active());
}

TEST(key_after_enter_cancels_the_launch) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_roms(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP);
    poll(0, UI_KEY_DOWN, -1, 15);
    txn = 0;
    uint8_t g = 'G';
    ui_feed(&g, 1, 20);
    ASSERT_EQ(tx[0], UI_GO_FAIL);
    ASSERT(last_line[0] == '\0');
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof tmpl, "%s/uiXXXXXX", tmpdir);
    snprintf(g_dir, sizeof g_dir, "%s", mkdtemp(tmpl));
    plat_host_set_dir(g_dir);

    RUN(home_screen_shows_drives_and_menu);
    RUN(inactive_until_ctl);
    RUN(first_poll_clears_and_draws);
    RUN(caps_shown);
    RUN(coco3_header_shows_ram_size);
    RUN(launch_reply_clears_the_screen);
    RUN(break_reply_clears_the_screen);
    RUN(coco3_break_reply_clears_the_screen);
    RUN(second_poll_sends_only_changes);
    RUN(resend_repeats_last_reply);
    RUN(bad_poll_gets_no_reply);
    RUN(half_poll_recovers_after_quiet);
    RUN(reply_never_exceeds_cap);
    RUN(no_roms_message_and_enter_is_harmless);
    RUN(lists_roms_sorted_and_hides_others);
    RUN(awkward_names_hidden);
    RUN(scrolls_with_the_selection);
    RUN(more_than_128_shows_truncated);
    RUN(enter_on_pak_asks_for_jump_then_go_loads);
    RUN(dos_rom_needs_ecb);
    RUN(dos_rom_with_ecb_cold_restarts);
    RUN(coco3_pak_uses_the_rom_mode_jump);
    RUN(coco3_dos_rom_uses_the_coco3_cold_restart);
    RUN(coco3_break_uses_the_coco3_warm_restart);
    RUN(coco2_codes_unchanged);
    RUN(default_rom_is_set_and_saved);
    RUN(launch_is_not_saved);
    RUN(loader_request_launches_the_manager);
    RUN(loader_entry_break_without_previous_rom);
    RUN(mounting_an_image_held_by_another_drive_moves_it);
    RUN(disks_lists_images_not_roms_or_config);
    RUN(mount_and_eject_are_saved_and_shown);
    RUN(failed_change_shows_the_reason_and_does_not_save);
    RUN(boot_lists_programs_from_the_directory);
    RUN(boot_full_directory_has_no_end_marker);
    RUN(boot_with_no_programs_stays_on_disks);
    RUN(boot_refused_in_bridge_and_net_mode);
    RUN(boot_os9_disk_loads_the_boot_track);
    RUN(boot_os9_disk_needs_64k);
    RUN(boot_cannot_read_an_unmounted_drive);
    RUN(boot_on_a_loader_entry_types_the_line);
    RUN(boot_on_a_power_on_entry_offers_the_dos);
    RUN(boot_on_a_coco3_power_on_entry_cold_restarts);
    RUN(boot_program_needs_extended_basic);
    RUN(boot_on_a_loader_entry_swaps_the_dos_back);
    RUN(boot_ignores_a_non_rsdos_directory);
    RUN(loader_entry_prev_is_dropped_on_a_jump_leave);
    RUN(boot_break_returns_to_disks_with_drive_0_mounted);
    RUN(new_disk_keeps_case_and_adds_dsk);
    RUN(new_disk_with_an_extension_is_left_alone);
    RUN(editor_limit_and_cancel);
    RUN(new_disk_name_must_fit_the_list);
    RUN(wifi_psk_is_masked_and_saved);
    RUN(wifi_scan_pick_and_join_saves_once);
    RUN(wifi_scan_failure_is_shown);
    RUN(wifi_server_host_then_port);
    RUN(wifi_server_bad_port_runs_nothing);
    RUN(clock_is_set_from_two_prompts);
    RUN(clock_break_on_either_prompt_writes_nothing);
    RUN(bad_clock_text_runs_nothing);
    RUN(timezone_is_parsed_in_minutes);
    RUN(timezone_picker_enter_sets_and_saves);
    RUN(timezone_picker_preselects_the_current_offset);
    RUN(timezone_custom_falls_through_to_the_prompt);
    RUN(settings_names_the_zone_of_the_offset);
    RUN(list_pages_counts_and_jumps);
    RUN(g_then_an_arrow_key_moves);
    RUN(g_then_a_control_key_still_acts);
    RUN(eject_arm_lasts_one_key);
    RUN(new_session_starts_without_last_sessions_message);
    RUN(disks_lists_image_files_only);
    RUN(home_long_read_only_name_keeps_ro);
    RUN(settings_toggle_hdbdos_is_saved);
    RUN(wifi_screen_without_a_radio_is_one_line);
    RUN(bad_size_refused_before_leaving);
    RUN(go_failure_keeps_session_and_shows_reason);
    RUN(go_without_a_pending_launch_fails);
    RUN(break_asks_for_warm_restart);
    RUN(key_after_enter_cancels_the_launch);
    TEST_MAIN_END
}
