#include "test.h"
#include "dw_vser.h"
#include <stdlib.h>

static dw_vser v;
static char last_line[VSER_LINE_MAX];
static int exec_calls;
static int fake_rc;
static const char *fake_out;

static int fake_exec(void *ctx, const char *line, char *out, size_t cap, size_t *outn) {
    (void)ctx;
    exec_calls++;
    snprintf(last_line, sizeof(last_line), "%s", line);
    size_t n = strlen(fake_out);
    if (n > cap) n = cap;
    memcpy(out, fake_out, n);
    *outn = n;
    return fake_rc;
}

static void setup(void) {
    vser_init(&v, fake_exec, NULL);
    exec_calls = 0; fake_rc = 0; fake_out = "hello\n"; last_line[0] = '\0';
}

static void wr(uint8_t ch, const char *s) { vser_write(&v, ch, (const uint8_t *)s, strlen(s)); }

/* Drains channel ch the way a client does; returns bytes read, sets *hung. */
static size_t drain(uint8_t ch, uint8_t *buf, size_t cap, int *hung) {
    size_t n = 0; *hung = 0;
    for (int guard = 0; guard < 10000; guard++) {
        uint8_t r[2];
        vser_serread(&v, r);
        if (r[0] == 0x10 && r[1] == ch) { *hung = 1; return n; }
        if (r[0] == 0 && r[1] == 0) return n;
        if (r[0] == ch + 1) { if (n < cap) buf[n++] = r[1]; continue; }
        if (r[0] == ch + 17) {
            uint8_t tmp[256];
            size_t got = vser_serreadm(&v, ch, r[1], tmp);
            if (got != r[1]) return n;
            for (size_t i = 0; i < got && n < cap; i++) buf[n++] = tmp[i];
            continue;
        }
        return n;
    }
    return n;
}

TEST(idle_serread_is_zero) {
    setup();
    uint8_t r[2] = {9, 9};
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0); ASSERT_EQ(r[1], 0);
}

TEST(write_without_open_is_dropped) {
    setup();
    wr(1, "version\r");
    ASSERT_EQ(exec_calls, 0);
}

TEST(ok_reply_framing_and_hangup) {
    setup();
    vser_open(&v, 1);
    wr(1, "version\r");
    ASSERT_EQ(exec_calls, 1);
    ASSERT(strcmp(last_line, "version") == 0);
    uint8_t buf[512]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    const char *want = "OK command successful\n\rhello\n";
    ASSERT_EQ(n, strlen(want));
    ASSERT_MEMEQ(buf, want, n);
    ASSERT(hung);
    ASSERT_EQ(v.ch, 0);
}

TEST(fail_reply_framing) {
    setup();
    fake_rc = 10; fake_out = "usage: dw hdbdos on|off";
    vser_open(&v, 1);
    wr(1, "dw hdbdos\r");
    uint8_t buf[512]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    const char *want = "FAIL 010 usage: dw hdbdos on|off\n\r";
    ASSERT_EQ(n, strlen(want));
    ASSERT_MEMEQ(buf, want, n);
    ASSERT(hung);
}

TEST(serread_small_is_single_byte_large_is_bulk) {
    setup();
    fake_out = "";   /* body empty: queue is just the 23-byte OK line */
    vser_open(&v, 3);
    wr(3, "x\r");
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 3 + 17); ASSERT_EQ(r[1], 23);
    uint8_t tmp[256];
    ASSERT_EQ(vser_serreadm(&v, 3, 21, tmp), 21);
    vser_serread(&v, r);            /* 2 left: single-byte mode */
    ASSERT_EQ(r[0], 3 + 1); ASSERT_EQ(r[1], '\n');
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 3 + 1); ASSERT_EQ(r[1], '\r');
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0x10); ASSERT_EQ(r[1], 3);
}

TEST(bulk_count_caps_at_255) {
    setup();
    static char big[1000];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    fake_out = big;
    vser_open(&v, 1);
    wr(1, "x\r");
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 18); ASSERT_EQ(r[1], 255);
}

TEST(serreadm_short_or_wrong_channel_sends_nothing) {
    setup();
    vser_open(&v, 1);
    wr(1, "x\r");
    uint8_t tmp[256];
    ASSERT_EQ(vser_serreadm(&v, 2, 5, tmp), 0);
    ASSERT_EQ(vser_serreadm(&v, 1, 200, tmp), 0);   /* only 29 queued */
    ASSERT_EQ(v.qlen, 29);
}

TEST(line_editing) {
    setup();
    vser_open(&v, 1);
    wr(1, "  verx");
    uint8_t bs = 0x08, nul = 0, lf = '\n';
    vser_write(&v, 1, &bs, 1);
    vser_write(&v, 1, &nul, 1);
    wr(1, "sion  ");
    vser_write(&v, 1, &lf, 1);
    ASSERT_EQ(exec_calls, 0);
    wr(1, "\r");
    ASSERT_EQ(exec_calls, 1);
    ASSERT(strcmp(last_line, "version") == 0);
}

TEST(blank_line_ignored) {
    setup();
    vser_open(&v, 1);
    wr(1, "   \r");
    ASSERT_EQ(exec_calls, 0);
    wr(1, "status\r");
    ASSERT_EQ(exec_calls, 1);
}

TEST(overlong_line_fails_010) {
    setup();
    vser_open(&v, 1);
    char l[300];
    memset(l, 'a', 299); l[299] = '\0';
    wr(1, l);
    wr(1, "\r");
    ASSERT_EQ(exec_calls, 0);
    uint8_t buf[256]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    buf[n] = 0;
    ASSERT(strncmp((char *)buf, "FAIL 010 line too long\n\r", n) == 0);
    ASSERT(hung);
}

TEST(writes_after_command_dropped) {
    setup();
    vser_open(&v, 1);
    wr(1, "a\rb\r");
    ASSERT_EQ(exec_calls, 1);
}

TEST(second_channel_rejected) {
    setup();
    vser_open(&v, 1);
    vser_open(&v, 2);
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0x10); ASSERT_EQ(r[1], 2);
    wr(1, "version\r");                 /* channel 1 undisturbed */
    uint8_t buf[512]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    ASSERT(n > 0 && hung);
}

TEST(bad_channels_ignored) {
    setup();
    vser_open(&v, 0);
    vser_open(&v, 14);
    vser_open(&v, 15);
    ASSERT_EQ(v.ch, 0);
}

TEST(open_count_and_close) {
    setup();
    vser_open(&v, 1);
    vser_open(&v, 1);
    vser_close(&v, 1);
    ASSERT_EQ(v.ch, 1);
    vser_close(&v, 1);
    ASSERT_EQ(v.ch, 0);
}

TEST(reset_drops_session) {
    setup();
    vser_open(&v, 1);
    wr(1, "x\r");
    vser_reset(&v);
    uint8_t r[2];
    vser_serread(&v, r);
    ASSERT_EQ(r[0], 0); ASSERT_EQ(r[1], 0);
}

TEST(reopen_after_hangup_is_clean) {
    setup();
    vser_open(&v, 1);
    wr(1, "x");                  /* partial line, then client vanishes (BREAK) */
    vser_close(&v, 1);
    vser_open(&v, 1);
    wr(1, "status\r");
    ASSERT(strcmp(last_line, "status") == 0);
}

TEST(no_exec_handler_fails_255) {
    vser_init(&v, NULL, NULL);
    vser_open(&v, 1);
    wr(1, "x\r");
    uint8_t buf[256]; int hung;
    size_t n = drain(1, buf, sizeof(buf), &hung);
    buf[n] = 0;
    ASSERT(strncmp((char *)buf, "FAIL 255 no command handler\n\r", n) == 0);
}

TEST(fail_message_cut_to_80) {
    setup();
    static char longmsg[200];
    memset(longmsg, 'm', 199); longmsg[199] = '\0';
    fake_rc = 255; fake_out = longmsg;
    vser_open(&v, 1);
    wr(1, "x\r");
    ASSERT_EQ(v.qlen, 9 + 80 + 2);
}

int main(void) {
    RUN(idle_serread_is_zero);
    RUN(write_without_open_is_dropped);
    RUN(ok_reply_framing_and_hangup);
    RUN(fail_reply_framing);
    RUN(serread_small_is_single_byte_large_is_bulk);
    RUN(bulk_count_caps_at_255);
    RUN(serreadm_short_or_wrong_channel_sends_nothing);
    RUN(line_editing);
    RUN(blank_line_ignored);
    RUN(overlong_line_fails_010);
    RUN(writes_after_command_dropped);
    RUN(second_channel_rejected);
    RUN(bad_channels_ignored);
    RUN(open_count_and_close);
    RUN(reset_drops_session);
    RUN(reopen_after_hangup_is_clean);
    RUN(no_exec_handler_fails_255);
    RUN(fail_message_cut_to_80);
    TEST_MAIN_END
}
