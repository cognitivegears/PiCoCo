#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "dw_store.h"
#include "dw_disk.h"
#include "dw_util.h"
#include "dw_vser.h"

#define DW_MAX_DRIVES 4
#define DW_PAYLOAD_TIMEOUT_MS 250
#define DW_HDBDOS_DISK_SECTORS 630

/* DriveWire protocol version we advertise in the DWINIT reply (DW4 sends 4). */
#define DW_PROTOCOL_VERSION 0x04

/* DriveWire opcodes (pyDriveWire dwserver.py, plus DW4 extras noted below),
 * ascending by value. */
#define DW_OP_NOP            0x00
#define DW_OP_NAMEOBJ_MOUNT  0x01
#define DW_OP_NAMEOBJ_CREATE 0x02
#define DW_OP_NAMEOBJ_TYPE   0x03 /* DW4 */
#define DW_OP_TIME           0x23
#define DW_OP_SETTIME        0x24 /* DW4 */
#define DW_OP_TIMER          0x25 /* DW4 */
#define DW_OP_RESET_TIMER    0x26 /* DW4 */
#define DW_OP_AARON          0x41 /* DW4; no payload, no reply */
#define DW_OP_WIREBUG_MODE   0x42 /* DW4; 23-byte payload, consumed, no reply */
#define DW_OP_SERREAD        0x43
#define DW_OP_SERGETSTAT     0x44
#define DW_OP_SERINIT        0x45
#define DW_OP_PRINTFLUSH     0x46
#define DW_OP_GETSTAT        0x47
#define DW_OP_INIT           0x49
#define DW_OP_PRINT          0x50
#define DW_OP_READ           0x52
#define DW_OP_SETSTAT        0x53
#define DW_OP_TERM           0x54
#define DW_OP_WRITE          0x57
#define DW_OP_DWINIT         0x5A
#define DW_OP_SERREADM       0x63
#define DW_OP_SERWRITEM      0x64
#define DW_OP_REREAD         0x72
#define DW_OP_REWRITE        0x77
#define DW_OP_FASTWRITE_BASE 0x80 /* .. 0x8F */
#define DW_OP_FASTWRITE_WINDOW_BASE 0x90 /* .. 0x9D; DW4 fast-write to window ports */
#define DW_OP_SERWRITE       0xC3
#define DW_OP_SERSETSTAT     0xC4
#define DW_OP_SERTERM        0xC5
#define DW_OP_READEX         0xD2
/* DW_OP_RFM (0xD6) is deliberately left unrecognized: it is a variable-length
 * sub-protocol (remote file manager) we do not implement. */
#define DW_OP_230K230K       0xE6 /* DW4; no payload, no reply */
#define DW_OP_REREADEX       0xF2
#define DW_OP_RESET3         0xF8
#define DW_OP_230K115K       0xFD /* DW4; no payload, no reply */
#define DW_OP_RESET2         0xFE
#define DW_OP_RESET1         0xFF

typedef void (*dw_send_fn)(void *ctx, const uint8_t *buf, size_t n);
typedef void (*dw_capture_fn)(void *ctx, int dir /*0 rx,1 tx*/, const uint8_t *buf, size_t n);

typedef struct {
    uint32_t ops[256], reads, writes, read_err, write_err, crc_err, timeouts, unknown_op, notrdy;
} dw_stats;

typedef enum { DW_IDLE, DW_PAYLOAD, DW_READEX_CKSUM } dw_state;

typedef struct dw_server {
    dw_store *store; dw_send_fn send; void *send_ctx;
    dw_capture_fn capture; void *capture_ctx;
    dw_disk drives[DW_MAX_DRIVES];
    bool hdbdos;
    dw_state state; uint8_t op; uint8_t buf[264]; uint16_t have, need;
    uint32_t last_rx_ms;
    uint16_t sector_sum; uint8_t pending_rc;
    int64_t time_base;      /* unix seconds at time_base_ms */
    uint32_t time_base_ms;
    dw_stats stats;
    dw_vser vser;
} dw_server;

void dw_init(dw_server *s, dw_store *store, dw_send_fn send, void *ctx);
void dw_feed(dw_server *s, const uint8_t *buf, size_t n, uint32_t now_ms);
void dw_tick(dw_server *s, uint32_t now_ms);
int  dw_mount(dw_server *s, int drive, const char *name, bool read_only);   /* dw_disk_open result */
void dw_eject(dw_server *s, int drive);
void dw_time_set(dw_server *s, int64_t unix_secs, uint32_t now_ms);
int64_t dw_time_get(dw_server *s, uint32_t now_ms);
void dw_set_capture(dw_server *s, dw_capture_fn fn, void *ctx);
/* Command handler for vserial lines. Runs inside dw_feed's dispatch (called
 * synchronously from vser_write/etc.); the callback must not re-enter
 * dw_feed (e.g. "dw selftest" must never be remote-callable). */
void dw_set_exec(dw_server *s, vser_exec_fn fn, void *ctx);
