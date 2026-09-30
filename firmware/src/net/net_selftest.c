#include "net_selftest.h"
#include "net.h"
#include "mode.h"
#include "fake6809.h"
#include "plat.h"
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>

/* Push one DriveWire request through the real path (fake 6809 -> core1 ->
 * Becker ring -> mode pump -> net -> server -> back) and collect the reply.
 * The console runs inside main's loop, so the pumps are driven here. */
static dw_server *dw_of_mode;   /* mode_pump needs the server only in NATIVE; NULL is fine in NET */

static int becker_wait_byte(uint8_t *out, uint32_t timeout_ms) {
    uint32_t t0 = plat_now_ms();
    for (;;) {
        mode_pump(dw_of_mode, plat_now_ms());
        net_poll(plat_now_ms());
        watchdog_update();
        if (fake6809_cycle(0xFF41, true, 0) == 0x02) { *out = fake6809_cycle(0xFF42, true, 0); return 0; }
        if (plat_now_ms() - t0 > timeout_ms) return -1;
    }
}

static int transact(const uint8_t *req, size_t rn, uint8_t *rep, size_t want, uint32_t *rtt_ms) {
    uint32_t t0 = plat_now_ms();
    for (size_t i = 0; i < rn; i++) fake6809_cycle(0xFF42, false, req[i]);
    for (size_t i = 0; i < want; i++) if (becker_wait_byte(&rep[i], 2000) != 0) return -1;
    *rtt_ms = plat_now_ms() - t0;
    return 0;
}

int net_selftest(void (*line)(const char *s)) {
    char buf[96];
    if (mode_get() != MODE_NET || net_state() != NET_UP) return -2;
    int b = fake6809_begin();
    if (b != 0) return b;
    int fails = 0;
    uint8_t rep[8]; uint32_t rtt;
    const uint8_t dwinit[2] = { 0x5A, 0xFF };   /* DWINIT takes a driver-id byte; >= 0x80 leaves HDB-DOS mode alone */
    if (transact(dwinit, 2, rep, 1, &rtt) == 0) { snprintf(buf, sizeof buf, "selftest net dwinit -> %02x in %u ms", rep[0], (unsigned)rtt); line(buf); }
    else { line("selftest net dwinit FAIL (no reply)"); fails++; }
    const uint8_t optime[1] = { 0x23 };
    uint32_t worst = 0;
    for (int i = 0; i < 20; i++) {
        if (transact(optime, 1, rep, 6, &rtt) != 0) { line("selftest net time FAIL (no reply)"); fails++; break; }
        if (rtt > worst) worst = rtt;
        if (i == 0) { snprintf(buf, sizeof buf, "selftest net time (yr-1900) %u-%02u-%02u %02u:%02u:%02u in %u ms", rep[0], rep[1], rep[2], rep[3], rep[4], rep[5], (unsigned)rtt); line(buf); }
    }
    snprintf(buf, sizeof buf, "selftest net time worst %u ms over 20", (unsigned)worst); line(buf);
    if (worst > 200) { line("selftest net FAIL: worst round trip over 200 ms (power save?)"); fails++; }
    if (!fails && (rep[0] < 126 || rep[0] > 199)) { line("selftest net FAIL: year out of range"); fails++; }
    fake6809_end();
    return fails ? -1 : 0;
}
