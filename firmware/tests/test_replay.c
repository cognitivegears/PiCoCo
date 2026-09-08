/* Regression test: feed every recorded capture fixture's rx chunks back
 * through a fresh dw_server and check the tx bytes it produces still match
 * the tx chunks the fixture recorded. See tests/fixtures/README.md for the
 * chunk format. */
#include "test.h"
#include "dw.h"
#include "dw_store.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef FIXTURE_DIR
#error "FIXTURE_DIR not defined (see CMakeLists.txt)"
#endif

static char g_dir[256];

static uint8_t out_buf[65536];
static size_t out_len;

static void on_send(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    if (out_len + n > sizeof(out_buf)) n = sizeof(out_buf) - out_len;
    memcpy(out_buf + out_len, buf, n);
    out_len += n;
}

/* Sector n's byte 0 is n & 0xFF, byte 1 is n >> 8, rest zero (matches
 * test_stack.c / test_console.c). */
static void mk_raw_dsk(void) {
    char path[512];
    snprintf(path, sizeof(path), "%s/raw.dsk", g_dir);
    FILE *fp = fopen(path, "wb");
    uint8_t sec[256];
    for (int n = 0; n < 630; n++) {
        memset(sec, 0, sizeof(sec));
        sec[0] = (uint8_t)(n & 0xFF);
        sec[1] = (uint8_t)(n >> 8);
        fwrite(sec, 1, sizeof(sec), fp);
    }
    fclose(fp);
}

/* Replays one fixture file; returns 0 ok, -1 on a test failure (already
 * reported via ASSERT). */
static void replay_one(const char *path) {
    FILE *f = fopen(path, "rb");
    ASSERT(f != NULL);

    mk_raw_dsk(); /* fresh per fixture: a WRITE fixture must not leak into the next */
    out_len = 0;
    dw_store store;
    dw_store_posix_init(&store, g_dir);
    dw_server dw;
    dw_init(&dw, &store, on_send, NULL);
    dw.hdbdos = false;
    ASSERT_EQ(dw_mount(&dw, 0, "raw.dsk", false), 0);

    uint8_t expected[65536];
    size_t expected_len = 0;

    uint8_t hdr[3];
    while (fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr)) {
        int dir = hdr[0];
        ASSERT(dir == 0 || dir == 1);
        uint16_t len = (uint16_t)(hdr[1] | (hdr[2] << 8));
        uint8_t chunk[264];
        ASSERT(len <= sizeof(chunk));
        size_t got = fread(chunk, 1, len, f);
        ASSERT_EQ(got, len);

        if (dir == 0) {
            dw_feed(&dw, chunk, len, 0);
            dw_tick(&dw, 0);
        } else {
            ASSERT(expected_len + len <= sizeof(expected));
            memcpy(expected + expected_len, chunk, len);
            expected_len += len;
        }
    }
    fclose(f);

    ASSERT_EQ(dw.stats.timeouts, 0);
    ASSERT_EQ(dw.stats.unknown_op, 0);
    ASSERT_EQ(out_len, expected_len);
    ASSERT_MEMEQ(out_buf, expected, expected_len);
}

TEST(all_fixtures_replay_identically) {
    DIR *d = opendir(FIXTURE_DIR);
    ASSERT(d != NULL);
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 4 || strcmp(ent->d_name + nlen - 4, ".cap") != 0) continue;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, ent->d_name);
        replay_one(path);
        found++;
    }
    closedir(d);
    ASSERT(found > 0);
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/replayXXXXXX", tmpdir);
    char *dir = mkdtemp(tmpl);
    snprintf(g_dir, sizeof(g_dir), "%s", dir);

    RUN(all_fixtures_replay_identically);
    TEST_MAIN_END
}
