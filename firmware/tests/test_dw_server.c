#include "test.h"
#include "dw.h"
#include "dw_store.h"
#include "dw_util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[256];
static dw_store st;
static dw_server s;

static uint8_t out[4096];
static size_t outn;

/* Writes hdr (hdrlen bytes) then nsect sectors where sector n's byte 0 is
 * n & 0xFF and byte 1 is n >> 8, rest zero. (copied from test_dw_disk.c) */
static void mk(const char *name, const uint8_t *hdr, int hdrlen, int nsect) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *fp = fopen(path, "wb");
    if (hdr && hdrlen > 0) fwrite(hdr, 1, (size_t)hdrlen, fp);
    uint8_t sec[256];
    for (int n = 0; n < nsect; n++) {
        memset(sec, 0, sizeof(sec));
        sec[0] = (uint8_t)(n & 0xFF);
        sec[1] = (uint8_t)(n >> 8);
        fwrite(sec, 1, sizeof(sec), fp);
    }
    fclose(fp);
}

static void on_send(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    memcpy(out + outn, buf, n);
    outn += n;
}

#define feed_at(now, ...) do { \
    uint8_t _b[] = { __VA_ARGS__ }; \
    dw_feed(&s, _b, sizeof(_b), (now)); \
} while (0)
#define feed(...) feed_at(0, __VA_ARGS__)

static void setup(void) {
    outn = 0;
    memset(out, 0, sizeof(out));
    dw_init(&s, &st, on_send, NULL);
    s.hdbdos = false;
    mk("raw.dsk", NULL, 0, 630);
    ASSERT_EQ(dw_mount(&s, 0, "raw.dsk", false), 0);
}

TEST(nop_and_reset_send_nothing) {
    setup();
    feed(0x00, 0xFF, 0xFE, 0xF8);
    ASSERT_EQ(outn, 0);
    ASSERT_EQ(s.stats.ops[0xFF], 1);
}

TEST(dwinit_replies_ff) {
    setup();
    feed(0x5A, 'A');
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 0xFF);
}

TEST(read_ok) {
    setup();
    feed(0x52, 0, 0, 0, 5);
    ASSERT_EQ(outn, 259);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ((out[1] << 8) | out[2], dw_checksum(out + 3, 256));
    ASSERT_EQ(out[3], 5);
}

TEST(read_unmounted_is_notrdy_with_full_reply) {
    setup();
    feed(0x52, 2, 0, 0, 0);
    ASSERT_EQ(outn, 259);
    ASSERT_EQ(out[0], DW_E_NOTRDY);
    ASSERT_EQ(out[1] | out[2], 0);
}

TEST(read_eof) {
    setup();
    feed(0x52, 0, 0, 2, 0x76); /* 630 */
    ASSERT_EQ(out[0], DW_E_EOF);
    ASSERT_EQ(outn, 259);
}

TEST(hdbdos_splits_drive_by_lsn) {
    setup();
    s.hdbdos = true;
    mk("raw2.dsk", NULL, 0, 630);
    ASSERT_EQ(dw_mount(&s, 1, "raw2.dsk", false), 0);
    feed(0x52, 0, 0, 2, 0x77); /* 631 -> drive1 lsn1 */
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[3], 1);
}

TEST(hdbdos_past_end_reads_zeros) {
    setup();
    s.hdbdos = true;
    mk("small.dsk", NULL, 0, 10);
    ASSERT_EQ(dw_mount(&s, 0, "small.dsk", false), 0);
    feed(0x52, 0, 0, 2, 0x00); /* 512 < 630, beyond a 10-sector image */
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(outn, 259);
}

TEST(write_ok_then_read_back) {
    setup();
    uint8_t d[256];
    memset(d, 0xA5, sizeof(d));
    uint16_t sum = dw_checksum(d, 256);
    uint8_t msg[1 + 1 + 3 + 256 + 2];
    msg[0] = 0x57; msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 3;
    memcpy(msg + 5, d, 256);
    msg[261] = (uint8_t)(sum >> 8);
    msg[262] = (uint8_t)(sum & 0xFF);
    dw_feed(&s, msg, sizeof(msg), 0);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 0);
    outn = 0;
    feed(0x52, 0, 0, 0, 3);
    ASSERT_EQ(out[3], 0xA5);
}

TEST(write_bad_checksum_is_crc_and_untouched) {
    setup();
    uint8_t d[256];
    memset(d, 0x5A, sizeof(d));
    uint16_t sum = dw_checksum(d, 256) ^ 1;
    uint8_t msg[1 + 1 + 3 + 256 + 2];
    msg[0] = 0x57; msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 3;
    memcpy(msg + 5, d, 256);
    msg[261] = (uint8_t)(sum >> 8);
    msg[262] = (uint8_t)(sum & 0xFF);
    dw_feed(&s, msg, sizeof(msg), 0);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], DW_E_CRC);
    ASSERT_EQ(s.stats.crc_err, 1);
    outn = 0;
    feed(0x52, 0, 0, 0, 3);
    ASSERT_EQ(out[3], 3); /* sector 3's original marker byte, untouched */
}

TEST(write_readonly_is_wrprot) {
    setup();
    ASSERT_EQ(dw_mount(&s, 3, "raw.dsk", true), 0);
    uint8_t d[256];
    memset(d, 0, sizeof(d));
    uint16_t sum = dw_checksum(d, 256);
    uint8_t msg[1 + 1 + 3 + 256 + 2];
    msg[0] = 0x57; msg[1] = 3; msg[2] = 0; msg[3] = 0; msg[4] = 0;
    memcpy(msg + 5, d, 256);
    msg[261] = (uint8_t)(sum >> 8);
    msg[262] = (uint8_t)(sum & 0xFF);
    dw_feed(&s, msg, sizeof(msg), 0);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], DW_E_WRPROT);
}

TEST(payload_split_across_feeds) {
    setup();
    feed(0x52, 0);
    feed(0, 0);
    feed(5);
    ASSERT_EQ(outn, 259);
}

TEST(payload_timeout_resets) {
    setup();
    feed_at(0, 0x52, 0, 0);
    dw_tick(&s, 300);
    ASSERT_EQ(s.state, DW_IDLE);
    ASSERT_EQ(s.stats.timeouts, 1);
    feed_at(300, 0x00);
    ASSERT_EQ(outn, 0);
}

TEST(unknown_op_ignored) {
    setup();
    feed(0x99);
    ASSERT_EQ(outn, 0);
    ASSERT_EQ(s.stats.unknown_op, 1);
    feed(0x5A, 0);
    ASSERT_EQ(outn, 1);
}

TEST(time_default_and_set) {
    setup();
    feed(0x23);
    ASSERT_EQ(outn, 6);
    ASSERT_EQ(out[0], 126);
    ASSERT_EQ(out[1], 1);
    ASSERT_EQ(out[2], 1);
    outn = 0;
    dw_time_set(&s, 1767225600 + 3661, 1000);
    feed_at(2000, 0x23);
    ASSERT_EQ(out[3], 1);
    ASSERT_EQ(out[4], 1);
    ASSERT_EQ(out[5], 2);
}

static uint8_t cap[64];
static size_t capn;
static void on_capture(void *ctx, int dir, const uint8_t *buf, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n && capn + 2 <= sizeof(cap); i++) {
        cap[capn++] = (uint8_t)dir;
        cap[capn++] = buf[i];
    }
}

TEST(capture_sees_both_directions) {
    setup();
    capn = 0;
    dw_set_capture(&s, on_capture, NULL);
    feed(0x5A, 'A');
    /* rx: 2 bytes (op + payload byte), then tx: 1 byte (0xFF reply) */
    ASSERT_EQ(capn, 6);
    ASSERT_EQ(cap[0], 0); ASSERT_EQ(cap[1], 0x5A);
    ASSERT_EQ(cap[2], 0); ASSERT_EQ(cap[3], 'A');
    ASSERT_EQ(cap[4], 1); ASSERT_EQ(cap[5], 0xFF);
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/dwservXXXXXX", tmpdir);
    char *d = mkdtemp(tmpl);
    strcpy(g_dir, d);
    dw_store_posix_init(&st, g_dir);

    RUN(nop_and_reset_send_nothing);
    RUN(dwinit_replies_ff);
    RUN(read_ok);
    RUN(read_unmounted_is_notrdy_with_full_reply);
    RUN(read_eof);
    RUN(hdbdos_splits_drive_by_lsn);
    RUN(hdbdos_past_end_reads_zeros);
    RUN(write_ok_then_read_back);
    RUN(write_bad_checksum_is_crc_and_untouched);
    RUN(write_readonly_is_wrprot);
    RUN(payload_split_across_feeds);
    RUN(payload_timeout_resets);
    RUN(unknown_op_ignored);
    RUN(time_default_and_set);
    RUN(capture_sees_both_directions);
    TEST_MAIN_END
}
