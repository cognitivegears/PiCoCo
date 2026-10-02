#include "test.h"
#include "ui.h"
#include "plat.h"
#include "dw_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[300];
static dw_store store;
static uint8_t tx[4096];
static size_t txn;
static char last_line[80];
static int exec_rc;

static void send_cb(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    memcpy(tx + txn, buf, n);
    txn += n;
}
static int exec_cb(const char *line, char *msg, size_t cap) {
    snprintf(last_line, sizeof last_line, "%s", line);
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
    dw_store_posix_init(&store, g_dir);
    ui_init(send_cb, NULL, exec_cb, &store);
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

TEST(coco3_header_has_no_ram_figure) {
    setup();
    ui_ctl(0xA5);
    poll(2, 0, UI_CAP_64K | UI_CAP_32K | UI_CAP_ECB | UI_CAP_COCO3, 0);
    char row[UI_COLS + 1];
    ui_row_text(0, row);
    ASSERT(strstr(row, "COCO 3") != NULL);
    ASSERT(strstr(row, "K ") == NULL);
    ASSERT(strstr(row, "ECB") == NULL);
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
    ui_ctl(0xA5);
    poll(2, 0, 0, 0);
    int len = reply_ok();
    ASSERT(len > 0 && len <= UI_REPLY_MAX);
    char row[UI_COLS + 1];
    ui_row_text(13, row);
    ASSERT(row[1] != ' ');                  /* the list really drew */
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

/* Review Focus 1 */
TEST(no_roms_message_and_enter_is_harmless) {
    setup();
    open_with(0);
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
    open_with(0);
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
    open_with(0);
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
    open_with(0);
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

/* Review Focus 2: which 64 of 70 are kept depends on directory order, so
 * only the notice is asserted. */
TEST(more_than_64_shows_truncated) {
    setup();
    char name[16];
    for (int i = 0; i < 70; i++) {
        snprintf(name, sizeof name, "r%02d.rom", i);
        mkfile(name, "\x7E\xC0\x10", 8192);
    }
    open_with(0);
    ASSERT(row_has(14, "LIST TRUNCATED"));
}

TEST(enter_on_pak_asks_for_jump_then_go_loads) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_with(0);
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
    open_with(0);                            /* no ECB */
    poll(0, UI_KEY_ENTER, -1, 10);
    ASSERT(row_has(14, "NEEDS EXTENDED BASIC"));
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 1], UI_ACT_END);
    ASSERT(len < 2 || tx[2 + len - 2] != UI_ACT_COLD);
}

TEST(dos_rom_with_ecb_cold_restarts) {
    setup();
    mkfile("hdb.rom", "DK", 8192);
    open_with(UI_CAP_32K | UI_CAP_ECB);
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
    open_with(CAPS_COCO3);
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
    open_with(CAPS_COCO3);
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

/* The CoCo 1/2 codes are unchanged. */
TEST(coco2_codes_unchanged) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_with(UI_CAP_32K | UI_CAP_ECB);
    poll(0, UI_KEY_ENTER, -1, 10);
    int len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_JUMP);
    poll(0, UI_KEY_BREAK, -1, 20);
    len = reply_ok();
    ASSERT_EQ(tx[2 + len - 2], UI_ACT_WARM);
}

TEST(bad_size_refused_before_leaving) {
    setup();
    mkfile("odd.rom", "\x7E\xC0\x10", 5000);
    open_with(0);
    poll(0, UI_KEY_ENTER, -1, 10);
    ASSERT(row_has(14, "NOT A ROM SIZE"));
    ASSERT(ui_active());
}

/* Review Focus 5 */
TEST(go_failure_keeps_session_and_shows_reason) {
    setup();
    mkfile("game.rom", "\x7E\xC0\x10", 8192);
    open_with(0);
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
    open_with(0);
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

    RUN(inactive_until_ctl);
    RUN(first_poll_clears_and_draws);
    RUN(caps_shown);
    RUN(coco3_header_has_no_ram_figure);
    RUN(second_poll_sends_only_changes);
    RUN(resend_repeats_last_reply);
    RUN(bad_poll_gets_no_reply);
    RUN(half_poll_recovers_after_quiet);
    RUN(reply_never_exceeds_cap);
    RUN(no_roms_message_and_enter_is_harmless);
    RUN(lists_roms_sorted_and_hides_others);
    RUN(awkward_names_hidden);
    RUN(scrolls_with_the_selection);
    RUN(more_than_64_shows_truncated);
    RUN(enter_on_pak_asks_for_jump_then_go_loads);
    RUN(dos_rom_needs_ecb);
    RUN(dos_rom_with_ecb_cold_restarts);
    RUN(coco3_pak_uses_the_rom_mode_jump);
    RUN(coco3_dos_rom_uses_the_coco3_cold_restart);
    RUN(coco3_break_uses_the_coco3_warm_restart);
    RUN(coco2_codes_unchanged);
    RUN(bad_size_refused_before_leaving);
    RUN(go_failure_keeps_session_and_shows_reason);
    RUN(go_without_a_pending_launch_fails);
    RUN(break_asks_for_warm_restart);
    RUN(key_after_enter_cancels_the_launch);
    TEST_MAIN_END
}
