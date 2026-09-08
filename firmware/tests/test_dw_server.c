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

/* Overwrites byte 2 of every sector with val, so two same-sized images can
 * be told apart by content (mk() alone makes them identical). */
static void stamp_byte2(const char *name, uint8_t val, int nsect) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *fp = fopen(path, "r+b");
    for (int n = 0; n < nsect; n++) {
        fseek(fp, n * 256 + 2, SEEK_SET);
        fwrite(&val, 1, 1, fp);
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
    /* RESET1/2/3 zero stats (item 6), so after all three the op counts
     * they themselves bumped are gone too. */
    ASSERT_EQ(s.stats.ops[0xFF], 0);
}

TEST(reset_zeroes_stats) {
    setup();
    feed(0x00);
    ASSERT_EQ(s.stats.ops[0x00], 1);
    feed(0xFF);
    ASSERT_EQ(s.stats.ops[0x00], 0);
    ASSERT_EQ(s.stats.ops[0xFF], 0);
    ASSERT_EQ(s.state, DW_IDLE);
}

TEST(dwinit_replies_version) {
    setup();
    feed(0x5A, 'A');
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 4);
}

TEST(dwinit_low_client_disables_hdbdos) {
    setup();
    s.hdbdos = true;
    feed(0x5A, 1);
    ASSERT(!s.hdbdos);
    s.hdbdos = true;
    feed(0x5A, 0x80);
    ASSERT(s.hdbdos);
}

TEST(read_ok) {
    setup();
    feed(0x52, 0, 0, 0, 5);
    ASSERT_EQ(outn, 259);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ((out[1] << 8) | out[2], dw_checksum(out + 3, 256));
    ASSERT_EQ(out[3], 5);
}

TEST(read_unmounted_is_one_byte) {
    setup();
    feed(0x52, 2, 0, 0, 0);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], DW_E_NOTRDY);
}

TEST(read_eof) {
    setup();
    /* raw.dsk (non-hdbdos) is exactly 630 sectors; LSN 630 is one past the
     * end and must come back as 256 zeros with rc 0 in every mode. */
    feed(0x52, 0, 0, 2, 0x76); /* 630 */
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(outn, 259);
    ASSERT_EQ((out[1] << 8) | out[2], 0);
    for (int i = 0; i < 256; i++) ASSERT_EQ(out[3 + i], 0);
}

TEST(readex_past_end_is_zeros) {
    setup();
    /* Swift testREADEX vector: LSN far beyond the image -> 256 zeros, rc 0. */
    feed(0xD2, 0, 0x00, 0x27, 0x10); /* LSN 10000 */
    ASSERT_EQ(outn, 256);
    for (int i = 0; i < 256; i++) ASSERT_EQ(out[i], 0);
    outn = 0;
    feed(0x00, 0x00); /* checksum of 256 zero bytes */
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 0);
}

TEST(hdbdos_splits_drive_by_lsn) {
    setup();
    s.hdbdos = true;
    mk("raw2.dsk", NULL, 0, 630);
    stamp_byte2("raw2.dsk", 0xB2, 630); /* distinguish from drive 0's raw.dsk */
    ASSERT_EQ(dw_mount(&s, 1, "raw2.dsk", false), 0);
    feed(0x52, 0, 0, 2, 0x77); /* 631 -> drive1 lsn1 */
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[3], 1);
    ASSERT_EQ(out[5], 0xB2);
}

TEST(hdbdos_drive_wrap_is_notrdy) {
    setup();
    s.hdbdos = true;
    /* LSN 161280 = 630*256: drive = lsn/630 = 256, which must NOT wrap into
     * range 0..3 (uint8_t truncation bug). LSN3 BE = 0x02,0x76,0x00. */
    uint8_t d[256];
    memset(d, 0x11, sizeof(d));
    uint16_t sum = dw_checksum(d, 256);
    uint8_t msg[1 + 1 + 3 + 256 + 2];
    msg[0] = 0x57; msg[1] = 0; msg[2] = 0x02; msg[3] = 0x76; msg[4] = 0x00;
    memcpy(msg + 5, d, 256);
    msg[261] = (uint8_t)(sum >> 8);
    msg[262] = (uint8_t)(sum & 0xFF);
    dw_feed(&s, msg, sizeof(msg), 0);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], DW_E_NOTRDY);

    outn = 0;
    feed(0x52, 0, 0x02, 0x76, 0x00);
    ASSERT_EQ(outn, 1); /* READ error reply: rc byte only */
    ASSERT_EQ(out[0], DW_E_NOTRDY);

    outn = 0;
    feed(0x52, 0, 0, 0, 0);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(out[3], 0); /* drive 0 sector 0's original marker byte, untouched */
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
    feed(0xA5); /* not in any recognized opcode or range */
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

TEST(time_survives_now_ms_wrap) {
    setup();
    dw_time_set(&s, 1767225600, 0xFFFFF000u);
    feed_at(0x00001000u, 0x23);
    ASSERT_EQ(outn, 6);
    ASSERT_EQ(out[3], 0);
    ASSERT_EQ(out[4], 0);
    ASSERT_EQ(out[5], 8);
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
    /* rx: 2 bytes (op + payload byte), then tx: 1 byte (version reply) */
    ASSERT_EQ(capn, 6);
    ASSERT_EQ(cap[0], 0); ASSERT_EQ(cap[1], 0x5A);
    ASSERT_EQ(cap[2], 0); ASSERT_EQ(cap[3], 'A');
    ASSERT_EQ(cap[4], 1); ASSERT_EQ(cap[5], 4);
}

TEST(readex_ok) {
    setup();
    feed(0xD2, 0, 0, 0, 5);
    ASSERT_EQ(outn, 256);
    ASSERT_EQ(out[0], 5);
    uint16_t sum = dw_checksum(out, 256);
    outn = 0;
    feed((uint8_t)(sum >> 8), (uint8_t)(sum & 0xFF));
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(s.state, DW_IDLE);
}

TEST(readex_bad_client_checksum) {
    setup();
    feed(0xD2, 0, 0, 0, 5);
    ASSERT_EQ(outn, 256);
    outn = 0;
    feed(0xFF, 0xFF);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], DW_E_CRC);
    ASSERT_EQ(s.stats.crc_err, 1);
    ASSERT_EQ(s.state, DW_IDLE);
}

TEST(readex_unmounted) {
    setup();
    feed(0xD2, 2, 0, 0, 0);
    ASSERT_EQ(outn, 256);
    for (int i = 0; i < 256; i++) ASSERT_EQ(out[i], 0);
    outn = 0;
    /* Deliberately wrong checksum for the all-zero data: proves rc stays
     * DW_E_NOTRDY (not E_CRC) because pending_rc != DW_E_OK skips the compare. */
    feed(0xFF, 0xFF);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], DW_E_NOTRDY);
}

TEST(readex_checksum_timeout_sends_nothing) {
    setup();
    feed_at(0, 0xD2, 0, 0, 0, 5);
    outn = 0;
    dw_tick(&s, 300);
    ASSERT_EQ(outn, 0);
    ASSERT_EQ(s.state, DW_IDLE);
    ASSERT_EQ(s.stats.timeouts, 1);
}

TEST(rereadex_same_as_readex) {
    setup();
    feed(0xF2, 0, 0, 0, 5);
    ASSERT_EQ(outn, 256);
    ASSERT_EQ(out[0], 5);
    uint16_t sum = dw_checksum(out, 256);
    outn = 0;
    feed((uint8_t)(sum >> 8), (uint8_t)(sum & 0xFF));
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 0);
    ASSERT_EQ(s.state, DW_IDLE);
}

TEST(serread_no_data) {
    setup();
    feed(0x43);
    ASSERT_EQ(outn, 2);
    ASSERT_EQ(out[0] | out[1], 0);
}

TEST(sersetstat_comst_consumes_26_more) {
    setup();
    feed(0xC4, 1, 0x28);
    ASSERT_EQ(outn, 0);
    uint8_t stat[26];
    memset(stat, 0x55, sizeof(stat));
    dw_feed(&s, stat, sizeof(stat), 0);
    ASSERT_EQ(outn, 0);
    ASSERT_EQ(s.stats.unknown_op, 0);
    feed(0x5A, 'A');
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 4);
}

TEST(sersetstat_other_code) {
    setup();
    feed(0xC4, 1, 0x29);
    ASSERT_EQ(outn, 0);
    feed(0x5A, 'A');
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 4);
}

TEST(serwritem_consumes_count) {
    setup();
    feed(0x64, 1, 3, 'a', 'b', 'c');
    ASSERT_EQ(outn, 0);
    feed(0x5A, 'A');
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 4);
}

TEST(nameobj_replies_zero) {
    setup();
    feed(0x01, 3, 'a', 'b', 'c');
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 0);
}

TEST(fastwrite_and_print_consumed) {
    setup();
    feed(0x81, 'x', 0x50, 'y', 0x46, 0x47, 0, 0, 0x53, 0, 0);
    ASSERT_EQ(outn, 0);
}

TEST(wirebug_packet_consumed) {
    setup();
    uint8_t pkt[24] = {0x42, 0x02, 0x08};
    dw_feed(&s, pkt, sizeof(pkt), 0);
    ASSERT_EQ(outn, 0);
    ASSERT_EQ(s.stats.unknown_op, 0);
    feed(0x5A, 0x01);
    ASSERT_EQ(outn, 1);
}

TEST(fastwrite_window_consumed) {
    setup();
    feed(0x91, 0x52);
    ASSERT_EQ(outn, 0);
    feed(0x5A, 0x01);
    ASSERT_EQ(outn, 1);
}

TEST(timer_replies_ms) {
    setup();
    feed_at(0x00010203u, 0x25, 0x00);
    ASSERT_EQ(outn, 4);
    ASSERT_EQ(out[0], 0x00);
    ASSERT_EQ(out[1], 0x01);
    ASSERT_EQ(out[2], 0x02);
    ASSERT_EQ(out[3], 0x03);
}

TEST(settime_sets_clock) {
    setup();
    uint32_t now_ms = 0x00010203u;
    feed_at(now_ms, 0x24, 0x7E, 0x09, 0x07, 0x0C, 0x22, 0x38);
    ASSERT_EQ(outn, 0);
    feed_at(now_ms, 0x23);
    ASSERT_EQ(outn, 6);
    ASSERT_EQ(out[0], 0x7E);
    ASSERT_EQ(out[1], 0x09);
    ASSERT_EQ(out[2], 0x07);
    ASSERT_EQ(out[3], 0x0C);
    ASSERT_EQ(out[4], 0x22);
    ASSERT_EQ(out[5], 0x38);
}

TEST(dw4_single_byte_ops_counted) {
    setup();
    feed(0x41, 0xE6, 0xFD);
    ASSERT_EQ(outn, 0);
    ASSERT_EQ(s.stats.unknown_op, 0);
    ASSERT_EQ(s.stats.ops[0x41], 1);
}

TEST(serreadm_replies_nothing) {
    setup();
    feed(0x63, 0x01, 0x04);
    ASSERT_EQ(outn, 0);
}

TEST(serwritem_count_zero_is_256) {
    setup();
    uint8_t pkt[3 + 256] = {0x64, 0x01, 0x00};
    dw_feed(&s, pkt, sizeof(pkt), 0);
    ASSERT_EQ(outn, 0);
    /* The 256 filler (0x00) bytes must be swallowed as SERWRITEM's data,
     * not re-parsed as 256 separate NOP opcodes. */
    ASSERT_EQ(s.stats.ops[0x00], 0);
    feed(0x5A, 0x01);
    ASSERT_EQ(outn, 1);
}

TEST(write_all_ff_checksum_is_ff00) {
    setup();
    uint8_t d[256];
    memset(d, 0xFF, sizeof(d));
    uint8_t msg[1 + 1 + 3 + 256 + 2];
    msg[0] = 0x57; msg[1] = 0; msg[2] = 0; msg[3] = 0; msg[4] = 4;
    memcpy(msg + 5, d, 256);
    msg[261] = 0xFF;
    msg[262] = 0x00;
    dw_feed(&s, msg, sizeof(msg), 0);
    ASSERT_EQ(outn, 1);
    ASSERT_EQ(out[0], 0);
}

TEST(mount_long_name_fails) {
    setup();
    const char *longname = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"; /* 40 chars, over dw_disk.name[32] */
    mk(longname, NULL, 0, 1);   /* file exists: without the length guard, open would succeed and truncate */
    ASSERT_EQ(dw_mount(&s, 1, longname, false), -1);
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
    RUN(reset_zeroes_stats);
    RUN(dwinit_replies_version);
    RUN(dwinit_low_client_disables_hdbdos);
    RUN(read_ok);
    RUN(read_unmounted_is_one_byte);
    RUN(read_eof);
    RUN(readex_past_end_is_zeros);
    RUN(hdbdos_splits_drive_by_lsn);
    RUN(hdbdos_drive_wrap_is_notrdy);
    RUN(hdbdos_past_end_reads_zeros);
    RUN(write_ok_then_read_back);
    RUN(write_bad_checksum_is_crc_and_untouched);
    RUN(write_readonly_is_wrprot);
    RUN(payload_split_across_feeds);
    RUN(payload_timeout_resets);
    RUN(unknown_op_ignored);
    RUN(time_default_and_set);
    RUN(time_survives_now_ms_wrap);
    RUN(capture_sees_both_directions);
    RUN(readex_ok);
    RUN(readex_bad_client_checksum);
    RUN(readex_unmounted);
    RUN(readex_checksum_timeout_sends_nothing);
    RUN(rereadex_same_as_readex);
    RUN(serread_no_data);
    RUN(sersetstat_comst_consumes_26_more);
    RUN(sersetstat_other_code);
    RUN(serwritem_consumes_count);
    RUN(nameobj_replies_zero);
    RUN(fastwrite_and_print_consumed);
    RUN(wirebug_packet_consumed);
    RUN(fastwrite_window_consumed);
    RUN(timer_replies_ms);
    RUN(settime_sets_clock);
    RUN(dw4_single_byte_ops_counted);
    RUN(serreadm_replies_nothing);
    RUN(serwritem_count_zero_is_256);
    RUN(write_all_ff_checksum_is_ff00);
    RUN(mount_long_name_fails);
    TEST_MAIN_END
}
