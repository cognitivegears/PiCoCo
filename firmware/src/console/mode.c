#include "mode.h"
#include "device.h"
#include "becker.h"
#include "plat.h"
#include <string.h>

mode_stats_t mode_stats;

static picoco_mode g_mode = MODE_DIAG;
static dw_server *g_bound_dw;

/* Pending DriveWire reply bytes not yet handed to becker_write. Sized for
 * two in-flight replies (259 bytes each): dw_feed() dispatches (and calls
 * mode_dw_send) synchronously for every complete request in whatever batch
 * it's given, with no chance to flush in between, so if becker_read() ever
 * hands it two full pipelined requests at once (a client that doesn't wait
 * for replies — DriveWire is normally request/reply) both land in `pending`
 * before mode_pump() gets to drain either. 1024 covers that; reply_overflow
 * stays as a should-never-happen guard beyond that. */
static uint8_t pending[1024];
static size_t pending_len;

void mode_bind(dw_server *dw) { g_bound_dw = dw; }

void mode_reset(void) {
    g_mode = MODE_DIAG;
    pending_len = 0;
    mode_stats.reply_overflow = 0;
}

void mode_set(picoco_mode m) {
    g_mode = m;
    if (m == MODE_NATIVE && g_bound_dw) g_bound_dw->state = DW_IDLE;
}

picoco_mode mode_get(void) { return g_mode; }

const char *mode_name(picoco_mode m) {
    switch (m) {
        case MODE_LOOP:   return "loop";
        case MODE_BRIDGE: return "bridge";
        case MODE_NATIVE: return "native";
        default:          return "off";
    }
}

void mode_dw_send(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    for (size_t i = 0; i < n; i++) {
        if (pending_len >= sizeof(pending)) {
            mode_stats.reply_overflow++;
            break;
        }
        pending[pending_len++] = buf[i];
    }
}

void mode_pump(dw_server *dw, uint32_t now_ms) {
    device_dispatch_writes();
    switch (g_mode) {
        case MODE_LOOP:
            becker_loopback_pump();
            break;
        case MODE_BRIDGE: {
            uint8_t buf[64];
            size_t n = becker_read(buf, sizeof(buf));
            if (n) plat_bridge_write(buf, n);
            size_t want = becker_tx_free();
            if (want > sizeof(buf)) want = sizeof(buf);
            if (want) {
                size_t got = plat_bridge_read(buf, want);
                if (got) becker_write(buf, got);
            }
            break;
        }
        case MODE_NATIVE: {
            /* Only feed more RX once the previous reply is fully flushed:
             * without this, a slow-draining reply plus fresh RX across
             * repeated calls could grow `pending` without bound. */
            if (pending_len == 0) {
                uint8_t buf[64];
                size_t n = becker_read(buf, sizeof(buf));
                if (n) dw_feed(dw, buf, n, now_ms);
            }
            if (pending_len > 0) {
                size_t free_n = becker_tx_free();
                size_t take = free_n < pending_len ? free_n : pending_len;
                if (take) {
                    size_t put = becker_write(pending, take);
                    memmove(pending, pending + put, pending_len - put);
                    pending_len -= put;
                }
            }
            dw_tick(dw, now_ms);
            break;
        }
        default:
            break;
    }
}
