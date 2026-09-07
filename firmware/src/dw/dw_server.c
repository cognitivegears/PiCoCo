#include "dw.h"
#include <string.h>
#include <time.h>

/* 2026-01-01 00:00:00 UTC */
#define DW_TIME_BASE_DEFAULT 1767225600

/* Sentinel: opcode not recognized at all (distinct from a real 0-byte payload). */
#define DW_UNKNOWN_LEN 0xFFFFu

static void tx(dw_server *s, const uint8_t *buf, size_t n) {
    if (s->capture) s->capture(s->capture_ctx, 1, buf, n);
    s->send(s->send_ctx, buf, n);
}

static uint16_t payload_len(uint8_t op) {
    switch (op) {
        case DW_OP_NOP:
        case DW_OP_TIME:
        case DW_OP_SERREAD:
        case DW_OP_PRINTFLUSH:
        case DW_OP_INIT:
        case DW_OP_TERM:
        case DW_OP_RESET3:
        case DW_OP_RESET2:
        case DW_OP_RESET1:
            return 0;
        case DW_OP_READ:
        case DW_OP_REREAD:
        case DW_OP_READEX:
        case DW_OP_REREADEX:
            return 4;
        case DW_OP_WRITE:
        case DW_OP_REWRITE:
            return 262;
        case DW_OP_NAMEOBJ_MOUNT:
        case DW_OP_NAMEOBJ_CREATE:
        case DW_OP_SERINIT:
        case DW_OP_PRINT:
        case DW_OP_DWINIT:
        case DW_OP_SERTERM:
            return 1;
        case DW_OP_SERGETSTAT:
        case DW_OP_GETSTAT:
        case DW_OP_SETSTAT:
        case DW_OP_SERREADM:
        case DW_OP_SERWRITEM:
        case DW_OP_SERWRITE:
        case DW_OP_SERSETSTAT:
            return 2;
        default:
            if (op >= DW_OP_FASTWRITE_BASE && op <= DW_OP_FASTWRITE_BASE + 0x0F) return 1;
            return DW_UNKNOWN_LEN;
    }
}

/* Resolves the target drive/lsn, applying the HDB-DOS drive-by-LSN split. */
static void resolve_drive(dw_server *s, uint8_t drive, uint32_t lsn, uint8_t *out_drive, uint32_t *out_lsn) {
    if (s->hdbdos) {
        drive = (uint8_t)(lsn / DW_HDBDOS_DISK_SECTORS);
        lsn %= DW_HDBDOS_DISK_SECTORS;
    }
    *out_drive = drive;
    *out_lsn = lsn;
}

static void do_read(dw_server *s, uint32_t now_ms) {
    (void)now_ms;
    uint8_t drive; uint32_t lsn;
    resolve_drive(s, s->buf[0], dw_lsn_unpack(&s->buf[1]), &drive, &lsn);

    uint8_t data[256] = {0};
    uint8_t rc;
    if (drive >= DW_MAX_DRIVES || !s->drives[drive].mounted) {
        rc = DW_E_NOTRDY;
        s->stats.notrdy++;
    } else {
        dw_disk *d = &s->drives[drive];
        if (lsn >= d->sectors) {
            /* HDB-DOS presents every drive as a full DW_HDBDOS_DISK_SECTORS
             * image; reads past the physical file are zeros, not EOF. */
            rc = s->hdbdos ? DW_E_OK : DW_E_EOF;
        } else {
            rc = (uint8_t)dw_disk_read(d, lsn, data);
            if (rc == DW_E_OK) s->stats.reads++;
            else if (rc == DW_E_READ) s->stats.read_err++;
        }
    }

    uint16_t sum = dw_checksum(data, 256);
    uint8_t reply[259];
    reply[0] = rc;
    reply[1] = (uint8_t)(sum >> 8);
    reply[2] = (uint8_t)sum;
    memcpy(reply + 3, data, 256);
    tx(s, reply, sizeof(reply));
}

static void do_write(dw_server *s, uint32_t now_ms) {
    (void)now_ms;
    const uint8_t *data = &s->buf[4];
    uint16_t want_sum = ((uint16_t)s->buf[260] << 8) | s->buf[261];
    uint16_t got_sum = dw_checksum(data, 256);

    uint8_t drive; uint32_t lsn;
    resolve_drive(s, s->buf[0], dw_lsn_unpack(&s->buf[1]), &drive, &lsn);

    uint8_t rc;
    if (got_sum != want_sum) {
        rc = DW_E_CRC;
        s->stats.crc_err++;
    } else if (drive >= DW_MAX_DRIVES || !s->drives[drive].mounted) {
        rc = DW_E_NOTRDY;
        s->stats.notrdy++;
    } else {
        rc = (uint8_t)dw_disk_write(&s->drives[drive], lsn, data);
        if (rc == DW_E_OK) s->stats.writes++;
        else if (rc == DW_E_WRITE) s->stats.write_err++;
    }
    tx(s, &rc, 1);
}

static void do_time(dw_server *s, uint32_t now_ms) {
    int64_t secs = s->time_base + ((int64_t)now_ms - (int64_t)s->time_base_ms) / 1000;
    time_t t = (time_t)secs;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    uint8_t reply[6] = {
        (uint8_t)tmv.tm_year, (uint8_t)(tmv.tm_mon + 1), (uint8_t)tmv.tm_mday,
        (uint8_t)tmv.tm_hour, (uint8_t)tmv.tm_min, (uint8_t)tmv.tm_sec,
    };
    tx(s, reply, sizeof(reply));
}

static void dispatch(dw_server *s, uint32_t now_ms) {
    switch (s->op) {
        case DW_OP_NOP:
        case DW_OP_TERM:
        case DW_OP_RESET1:
        case DW_OP_RESET2:
        case DW_OP_RESET3:
        case DW_OP_INIT:
            break;
        case DW_OP_DWINIT: {
            uint8_t r = 0xFF;
            tx(s, &r, 1);
            break;
        }
        case DW_OP_TIME:
            do_time(s, now_ms);
            break;
        case DW_OP_READ:
        case DW_OP_REREAD:
            do_read(s, now_ms);
            break;
        case DW_OP_WRITE:
        case DW_OP_REWRITE:
            do_write(s, now_ms);
            break;
        default:
            /* Recognized but not yet implemented (GETSTAT/SETSTAT, SERxxx,
             * NAMEOBJ, FASTWRITE, READEX/REREADEX) -- Task 4 extends this. */
            break;
    }
    s->state = DW_IDLE;
}

void dw_init(dw_server *s, dw_store *store, dw_send_fn send, void *ctx) {
    memset(s, 0, sizeof(*s));
    s->store = store;
    s->send = send;
    s->send_ctx = ctx;
    s->hdbdos = true;
    s->state = DW_IDLE;
    s->time_base = DW_TIME_BASE_DEFAULT;
    s->time_base_ms = 0;
}

void dw_feed(dw_server *s, const uint8_t *buf, size_t n, uint32_t now_ms) {
    if (s->capture) s->capture(s->capture_ctx, 0, buf, n);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = buf[i];
        s->last_rx_ms = now_ms;
        if (s->state == DW_IDLE) {
            s->op = b;
            uint16_t need = payload_len(b);
            if (need == DW_UNKNOWN_LEN) {
                s->stats.unknown_op++;
                continue;
            }
            s->stats.ops[b]++;
            s->need = need;
            s->have = 0;
            if (need == 0) {
                dispatch(s, now_ms);
            } else {
                s->state = DW_PAYLOAD;
            }
        } else if (s->state == DW_PAYLOAD) {
            s->buf[s->have++] = b;
            if (s->have >= s->need) dispatch(s, now_ms);
        }
    }
}

void dw_tick(dw_server *s, uint32_t now_ms) {
    if (s->state != DW_IDLE && now_ms - s->last_rx_ms > DW_PAYLOAD_TIMEOUT_MS) {
        s->state = DW_IDLE;
        s->stats.timeouts++;
    }
}

int dw_mount(dw_server *s, int drive, const char *name, bool read_only) {
    if (drive < 0 || drive >= DW_MAX_DRIVES) return -1;
    dw_disk_close(&s->drives[drive]); /* no-op if not mounted; avoids leaking the old fd on remount */
    return dw_disk_open(s->store, name, read_only, &s->drives[drive]);
}

void dw_eject(dw_server *s, int drive) {
    if (drive < 0 || drive >= DW_MAX_DRIVES) return;
    dw_disk_close(&s->drives[drive]);
}

void dw_time_set(dw_server *s, int64_t unix_secs, uint32_t now_ms) {
    s->time_base = unix_secs;
    s->time_base_ms = now_ms;
}

void dw_set_capture(dw_server *s, dw_capture_fn fn, void *ctx) {
    s->capture = fn;
    s->capture_ctx = ctx;
}
