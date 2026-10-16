#include "test.h"
#include "ui.h"
#include "mode.h"
#include "becker.h"
#include "bus.h"
#include "sim_bus.h"
#include "device.h"
#include "rom.h"
#include "console.h"
#include "dw.h"
#include "dw_store.h"
#include "log.h"
#include "plat.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static dw_server dw;
static dw_store store;
static char g_dir[300];
static void out_cb(void *ctx, const char *s) { (void)ctx; (void)s; }

static void setup(picoco_mode m) {
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
    log_init();
    mode_reset();
    dw_store_posix_init(&store, g_dir);
    dw_init(&dw, &store, mode_dw_send, NULL);
    console_init(out_cb, NULL, &dw, &store);
    net_forget();
    mode_set(m);
}

static void coco_writes(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) sim_write(BUS_IDX_BECKER_DATA, p[i]);
}

/* What the firmware queued for the CoCo, read the way the CoCo would. */
static size_t coco_reads(uint8_t *out, size_t cap) {
    size_t n = 0;
    for (int guard = 0; guard < 4000 && n < cap; guard++) {
        mode_pump(&dw, 0);
        sim_read(BUS_IDX_BECKER_STATUS);
        if (bus_table[BUS_IDX_BECKER_STATUS] != 0x02) { if (guard > 50 && n) break; continue; }
        out[n++] = bus_table[BUS_IDX_BECKER_DATA];
        sim_read(BUS_IDX_BECKER_DATA);
    }
    return n;
}

static const uint8_t first_poll[5] = { 'P', 2, 0, 0, (uint8_t)('P' + 2) };

TEST(ctl_write_starts_and_ends_a_session) {
    setup(MODE_NATIVE);
    ASSERT(!ui_active());
    sim_write(BUS_IDX_BECKER_CTL, 0xA5);
    mode_pump(&dw, 0);
    ASSERT(ui_active());
    sim_write(BUS_IDX_BECKER_CTL, 0x5A);
    mode_pump(&dw, 0);
    ASSERT(!ui_active());
}

TEST(poll_is_answered_in_native_mode) {
    setup(MODE_NATIVE);
    sim_write(BUS_IDX_BECKER_CTL, 0xA5);
    coco_writes(first_poll, sizeof first_poll);
    uint8_t got[700];
    size_t n = coco_reads(got, sizeof got);
    ASSERT(n > 4);
    size_t len = ((size_t)got[0] << 8) | got[1];
    ASSERT_EQ(n, len + 4);
    ASSERT_EQ(got[2], UI_ACT_CLEAR);
}

/* In bridge mode Becker bytes normally go to CDC0; a session keeps them. */
TEST(poll_is_answered_in_bridge_mode) {
    setup(MODE_BRIDGE);
    sim_write(BUS_IDX_BECKER_CTL, 0xA5);
    coco_writes(first_poll, sizeof first_poll);
    uint8_t got[700];
    size_t n = coco_reads(got, sizeof got);
    ASSERT(n > 4);
    ASSERT_EQ(got[2], UI_ACT_CLEAR);
}

/* Bytes written before the session began must not reach the UI parser. */
TEST(session_begin_drops_earlier_bytes) {
    setup(MODE_LOOP);
    const uint8_t stale[3] = { 'P', 'G', 'P' };
    coco_writes(stale, sizeof stale);
    sim_write(BUS_IDX_BECKER_CTL, 0xA5);
    coco_writes(first_poll, sizeof first_poll);
    uint8_t got[700];
    size_t n = coco_reads(got, sizeof got);
    size_t len = ((size_t)got[0] << 8) | got[1];
    ASSERT_EQ(n, len + 4);                   /* exactly one reply, nothing looped back */
}

TEST(drivewire_works_again_after_the_session) {
    setup(MODE_NATIVE);
    sim_write(BUS_IDX_BECKER_CTL, 0xA5);
    mode_pump(&dw, 0);
    sim_write(BUS_IDX_BECKER_CTL, 0x5A);
    const uint8_t dwinit[2] = { 0x5A, 0x00 };       /* OP_DWINIT, driver version */
    coco_writes(dwinit, sizeof dwinit);
    uint8_t got[8];
    ASSERT_EQ(coco_reads(got, sizeof got), 1);      /* one byte: the server's version */
}

TEST(exec_capture_returns_the_error_text) {
    setup(MODE_NATIVE);
    char msg[40];
    ASSERT_EQ(console_exec_capture("rom load nope.rom", msg, sizeof msg), -1);
    ASSERT(strcmp(msg, "rom load failed") == 0);
    ASSERT_EQ(console_exec_capture("rom pattern", msg, sizeof msg), 0);
}

/* #60: the Loader's L swaps the manager in and remembers the ROM it replaced;
 * BREAK on that entry (capability bit 3) puts it back before the return. */
TEST(loader_entry_restores_the_previous_rom_on_break) {
    setup(MODE_NATIVE);
    char path[400];
    snprintf(path, sizeof path, "%s/game.rom", g_dir);
    FILE *f = fopen(path, "wb");
    uint8_t img[8192] = { 0x7E, 0xC0, 0x10 };
    fwrite(img, 1, sizeof img, f);
    fclose(f);
    ASSERT_EQ(console_exec("rom load game.rom"), 0);
    ASSERT(!rom_is_dos());
    sim_write(BUS_IDX_BECKER_CTL, 0xA5);
    const uint8_t load[2] = { 'L', 'L' };
    coco_writes(load, sizeof load);
    uint8_t got[700];
    ASSERT_EQ(coco_reads(got, sizeof got), 1);
    ASSERT_EQ(got[0], UI_GO_OK);
    ASSERT(rom_is_dos());                           /* the manager image starts with DK */
    ASSERT(strcmp(ui_prev_rom(), "game.rom") == 0);
    const uint8_t first[6] = { 'P', 2, 0, UI_CAP_32K | UI_CAP_ECB | UI_CAP_DOS, 0, 0 };
    uint8_t p[5] = { first[0], first[1], first[2], first[3], (uint8_t)(first[0] + first[1] + first[2] + first[3]) };
    coco_writes(p, sizeof p);
    size_t n = coco_reads(got, sizeof got);
    ASSERT(n > 4);
    uint8_t brk[4] = { 'P', 0, UI_KEY_BREAK, (uint8_t)('P' + UI_KEY_BREAK) };
    coco_writes(brk, sizeof brk);
    n = coco_reads(got, sizeof got);
    size_t len = ((size_t)got[0] << 8) | got[1];
    ASSERT_EQ(n, len + 4);
    ASSERT_EQ(got[2 + len - 2], UI_ACT_WARM);
    const uint8_t go = 'G';
    coco_writes(&go, 1);
    ASSERT_EQ(coco_reads(got, sizeof got), 1);
    ASSERT_EQ(got[0], UI_GO_OK);
    ASSERT(!rom_is_dos());                          /* game.rom is back */
    ASSERT(ui_prev_rom()[0] == '\0');
    ASSERT(!ui_active());
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof tmpl, "%s/uisessXXXXXX", tmpdir);
    snprintf(g_dir, sizeof g_dir, "%s", mkdtemp(tmpl));
    plat_host_set_dir(g_dir);

    RUN(ctl_write_starts_and_ends_a_session);
    RUN(poll_is_answered_in_native_mode);
    RUN(poll_is_answered_in_bridge_mode);
    RUN(session_begin_drops_earlier_bytes);
    RUN(drivewire_works_again_after_the_session);
    RUN(exec_capture_returns_the_error_text);
    RUN(loader_entry_restores_the_previous_rom_on_break);
    TEST_MAIN_END
}
