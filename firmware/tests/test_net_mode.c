#include "test.h"
#include "mode.h"
#include "becker.h"
#include "bus.h"
#include "device.h"
#include "dw.h"
#include "dw_store.h"
#include "console.h"
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

static void setup(void) {
    bus_init(); device_reset(); becker_init(); device_init_all();
    log_init();
    mode_reset();
    dw_store_posix_init(&store, g_dir);
    dw_init(&dw, &store, mode_dw_send, NULL);
    console_init(out_cb, NULL, &dw, &store);
    net_forget();
    net_stub_set_state(NET_UP);
    mode_set(MODE_NET);
}

/* Bytes the CoCo writes to $FF42 come out of the to-server side. */
TEST(coco_writes_reach_server) {
    setup();
    for (int i = 0; i < 5; i++) bus_on_write(0x3F42, (uint8_t)(0x50 + i), 0);
    mode_pump(&dw, 0);
    uint8_t got[16];
    size_t n = net_stub_take_to_server(got, sizeof got);
    ASSERT_EQ(n, 5);
    ASSERT(got[0] == 0x50 && got[4] == 0x54);
}

/* Bytes from the server land in the Becker to-coco queue. */
TEST(server_bytes_reach_coco) {
    setup();
    const uint8_t reply[3] = { 1, 2, 3 };
    net_stub_push_from_server(reply, 3);
    mode_pump(&dw, 0);
    ASSERT_EQ(becker_tx_free(), 255 - 3);
}

/* Not up: net_write reports bytes consumed (bridge rule); the ring stays empty. */
TEST(not_up_consumes_quietly) {
    setup();
    net_stub_set_state(NET_CONNECTING);
    bus_on_write(0x3F42, 0x23, 0);
    mode_pump(&dw, 0);
    uint8_t got[4];
    ASSERT_EQ(net_stub_take_to_server(got, sizeof got), 0);
}

TEST(net_stub_reason_strings) {
    ASSERT(strcmp(net_state_name(NET_OFF), "off") == 0);
    ASSERT(strcmp(net_state_name(NET_JOINING), "joining") == 0);
    ASSERT(strcmp(net_state_name(NET_CONNECTING), "connecting") == 0);
    ASSERT(strcmp(net_state_name(NET_UP), "up") == 0);
    ASSERT(strcmp(net_state_name(NET_FAILED), "failed") == 0);
}

/* Reconfiguring while in net mode stops the link, drops to native, and
 * leaves nothing queued to replay after a later net round trip. */
TEST(mode_net_drops_to_native_on_reconfigure) {
    setup();
    const uint8_t reply[3] = { 1, 2, 3 };
    size_t empty = becker_tx_free();
    net_stub_push_from_server(reply, 3);
    bus_on_write(0x3F42, 0x11, 0);
    bus_on_write(0x3F42, 0x22, 0);
    ASSERT_EQ(console_exec("net server 1.2.3.4"), 0);
    ASSERT_EQ(mode_get(), MODE_NATIVE);
    net_stub_set_state(NET_UP);
    mode_set(MODE_NET);
    mode_pump(&dw, 0);
    uint8_t got[8];
    ASSERT_EQ(net_read(got, sizeof got), 0);
    ASSERT_EQ(becker_tx_free(), empty);
}

int main(void) {
    char tmpl[300];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(tmpl, sizeof(tmpl), "%s/netmodeXXXXXX", tmpdir);
    char *dir = mkdtemp(tmpl);
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    plat_host_set_dir(g_dir);
    RUN(coco_writes_reach_server);
    RUN(server_bytes_reach_coco);
    RUN(not_up_consumes_quietly);
    RUN(net_stub_reason_strings);
    RUN(mode_net_drops_to_native_on_reconfigure);
    TEST_MAIN_END
}
