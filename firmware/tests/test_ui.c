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
__attribute__((unused)) static int exec_cb(const char *line, char *msg, size_t cap) {
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
    RUN(second_poll_sends_only_changes);
    RUN(resend_repeats_last_reply);
    RUN(bad_poll_gets_no_reply);
    RUN(half_poll_recovers_after_quiet);
    RUN(reply_never_exceeds_cap);
    TEST_MAIN_END
}
