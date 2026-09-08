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
        case DW_OP_AARON:
        case DW_OP_230K230K:
        case DW_OP_230K115K:
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
        case DW_OP_NAMEOBJ_TYPE:
        case DW_OP_SERINIT:
        case DW_OP_PRINT:
        case DW_OP_DWINIT:
        case DW_OP_SERTERM:
        case DW_OP_TIMER:
        case DW_OP_RESET_TIMER:
            return 1;
        case DW_OP_SERGETSTAT:
        case DW_OP_GETSTAT:
        case DW_OP_SETSTAT:
        case DW_OP_SERREADM:
        case DW_OP_SERWRITEM:
        case DW_OP_SERWRITE:
        case DW_OP_SERSETSTAT:
            return 2;
        case DW_OP_SETTIME:
            return 6;
        case DW_OP_WIREBUG_MODE:
            return 23;
        default:
            if (op >= DW_OP_FASTWRITE_BASE && op <= DW_OP_FASTWRITE_BASE + 0x0F) return 1;
            /* DW4 only assigns 0x90..0x9D; 0x9E/0x9F stay unknown. */
            if (op >= DW_OP_FASTWRITE_WINDOW_BASE && op <= DW_OP_FASTWRITE_WINDOW_BASE + 0x0D) return 1;
            return DW_UNKNOWN_LEN;
    }
}

/* Days since 1970-01-01 for a proleptic-Gregorian civil date (Howard
 * Hinnant's algorithm). Avoids a libc timegm dependency on the Pico. */
static int64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Resolves the target drive/lsn, applying the HDB-DOS drive-by-LSN split. */
static void resolve_drive(dw_server *s, uint32_t drive, uint32_t lsn, uint32_t *out_drive, uint32_t *out_lsn) {
    if (s->hdbdos) {
        drive = lsn / DW_HDBDOS_DISK_SECTORS;
        lsn %= DW_HDBDOS_DISK_SECTORS;
    }
    *out_drive = drive;
    *out_lsn = lsn;
}

/* Resolves drive/lsn from a READ/READEX request (buf[0]=drive, buf[1..3]=lsn)
 * and reads the sector, so READ and READEX share one code path. */
static void read_sector(dw_server *s, const uint8_t *req, uint8_t *rc_out, uint8_t data[256]) {
    uint32_t drive; uint32_t lsn;
    resolve_drive(s, req[0], dw_lsn_unpack(&req[1]), &drive, &lsn);

    memset(data, 0, 256);
    uint8_t rc;
    if (drive >= DW_MAX_DRIVES || !s->drives[drive].mounted) {
        rc = DW_E_NOTRDY;
        s->stats.notrdy++;
    } else {
        rc = (uint8_t)dw_disk_read(&s->drives[drive], lsn, data);
        /* The spec defines no EOF-on-read: reads past the end of an image
         * come back as a blank (zeroed) sector with rc OK, in every mode.
         * data[] is already zeroed above; dw_disk_read leaves it untouched
         * on DW_E_EOF. */
        if (rc == DW_E_EOF) rc = DW_E_OK;
        if (rc == DW_E_OK) s->stats.reads++;
        else if (rc == DW_E_READ) s->stats.read_err++;
    }
    *rc_out = rc;
}

static void do_read(dw_server *s, uint32_t now_ms) {
    (void)now_ms;
    uint8_t rc, data[256];
    read_sector(s, s->buf, &rc, data);

    if (rc != DW_E_OK) {
        /* Read Failure packet: byte 0 error code only, no checksum/data. */
        tx(s, &rc, 1);
        return;
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

    uint32_t drive; uint32_t lsn;
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

/* UTC via gmtime_r; dw_time_set is the local-time calibration knob
 * (references use localtime). */
static void do_time(dw_server *s, uint32_t now_ms) {
    int64_t secs = s->time_base + (int64_t)(uint32_t)(now_ms - s->time_base_ms) / 1000;
    time_t t = (time_t)secs;
    struct tm tmv;
    uint8_t reply[6] = {0};
    if (gmtime_r(&t, &tmv)) {
        reply[0] = (uint8_t)tmv.tm_year; reply[1] = (uint8_t)(tmv.tm_mon + 1); reply[2] = (uint8_t)tmv.tm_mday;
        reply[3] = (uint8_t)tmv.tm_hour; reply[4] = (uint8_t)tmv.tm_min; reply[5] = (uint8_t)tmv.tm_sec;
    }
    tx(s, reply, sizeof(reply));
}

static void dispatch(dw_server *s, uint32_t now_ms) {
    switch (s->op) {
        case DW_OP_NOP:
        case DW_OP_TERM:
        case DW_OP_INIT:
        case DW_OP_SERGETSTAT:
        case DW_OP_SERINIT:
        case DW_OP_SERTERM:
        case DW_OP_SERWRITE:
        case DW_OP_PRINT:
        case DW_OP_PRINTFLUSH:
        case DW_OP_GETSTAT:
        case DW_OP_SETSTAT:
            break; /* consume, no reply */
        case DW_OP_RESET1:
        case DW_OP_RESET2:
        case DW_OP_RESET3: {
            /* spec: RESET flushes caches and resets statistics. */
            for (int i = 0; i < DW_MAX_DRIVES; i++) {
                dw_disk *d = &s->drives[i];
                if (d->mounted && d->store && d->store->ops && d->store->ops->sync)
                    d->store->ops->sync(&d->f);
            }
            memset(&s->stats, 0, sizeof(s->stats));
            break;
        }
        case DW_OP_DWINIT: {
            /* Clients sending a real drive number (< 0x80: NitrOS-9, CoCoBoot,
             * LWOS) mean HDB-DOS's drive-by-LSN split; turn it off for them. */
            if (s->buf[0] < 0x80) s->hdbdos = false;
            uint8_t r = DW_PROTOCOL_VERSION;
            tx(s, &r, 1);
            break;
        }
        case DW_OP_TIME:
            do_time(s, now_ms);
            break;
        case DW_OP_SETTIME: {
            int month = s->buf[1], day = s->buf[2], hour = s->buf[3], min = s->buf[4], sec = s->buf[5];
            /* ponytail: a malformed/adversarial client shouldn't be able to
             * feed garbage into gmtime_r via the clock; reject silently
             * rather than tracking a separate bad-arg counter. */
            if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59)
                break;
            int year = s->buf[0] + 1900;
            int64_t unix = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
            dw_time_set(s, unix, now_ms);
            break;
        }
        case DW_OP_TIMER: {
            uint8_t r[4] = {
                (uint8_t)(now_ms >> 24), (uint8_t)(now_ms >> 16),
                (uint8_t)(now_ms >> 8), (uint8_t)now_ms,
            };
            tx(s, r, sizeof(r));
            break;
        }
        case DW_OP_RESET_TIMER:
            /* ponytail: no per-timer state kept, so nothing to reset */
            break;
        case DW_OP_READ:
        case DW_OP_REREAD:
            do_read(s, now_ms);
            break;
        case DW_OP_WRITE:
        case DW_OP_REWRITE:
            do_write(s, now_ms);
            break;
        case DW_OP_READEX:
        case DW_OP_REREADEX: {
            uint8_t rc, data[256];
            read_sector(s, s->buf, &rc, data);
            tx(s, data, sizeof(data));
            s->pending_rc = rc;
            s->sector_sum = dw_checksum(data, sizeof(data));
            s->state = DW_READEX_CKSUM;
            s->have = 0;
            s->need = 2;
            return; /* stay out of the tail: not back to IDLE yet */
        }
        case DW_OP_SERREAD: {
            uint8_t r[2] = {0, 0};
            tx(s, r, sizeof(r));
            break;
        }
        case DW_OP_SERREADM:
            /* ponytail: no vserial channels to read from; spec says the
             * server sends nothing back when it can't supply the bytes
             * (count byte 0 would mean 256, but there's nothing to count). */
            break;
        case DW_OP_SERSETSTAT:
            /* buf = [chan, code]; COMST (0x28) carries 26 more status bytes. */
            if (s->have == 2 && s->buf[1] == 0x28) {
                s->need = 2 + 26;
                return;
            }
            break;
        case DW_OP_SERWRITEM:
            /* buf = [chan, count]; count byte 0 means 256 (a full block).
             * 2+256 always fits buf[264]. */
            if (s->have == 2) {
                uint16_t cnt = s->buf[1] ? s->buf[1] : 256;
                s->need = (uint16_t)(2 + cnt);
                return;
            }
            break;
        case DW_OP_NAMEOBJ_MOUNT:
        case DW_OP_NAMEOBJ_CREATE:
        case DW_OP_NAMEOBJ_TYPE: {
            /* buf = [len, ...name]; len <= 255 so 1+len always fits buf[264]. */
            if (s->have == 1 && s->buf[0] > 0) {
                s->need = (uint16_t)(1 + s->buf[0]);
                return;
            }
            uint8_t r = 0;
            tx(s, &r, 1);
            break;
        }
        default:
            /* FASTWRITE 0x80..0x8F, the DW4 fast-write window 0x90..0x9D,
             * AARON, WIREBUG_MODE, 230K230K/230K115K, and anything else
             * payload_len() recognizes: consume, no reply. */
            break;
    }
    s->state = DW_IDLE;
}

/* Finishes a READEX/REREADEX after the client's 2 checksum bytes arrive. */
static void finish_readex(dw_server *s) {
    uint16_t client_sum = ((uint16_t)s->buf[0] << 8) | s->buf[1];
    uint8_t rc = s->pending_rc;
    if (rc == DW_E_OK && client_sum != s->sector_sum) {
        rc = DW_E_CRC;
        s->stats.crc_err++;
    }
    tx(s, &rc, 1);
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
        } else if (s->state == DW_READEX_CKSUM) {
            s->buf[s->have++] = b;
            if (s->have >= s->need) finish_readex(s);
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

int64_t dw_time_get(dw_server *s, uint32_t now_ms) {
    return s->time_base + (int64_t)(uint32_t)(now_ms - s->time_base_ms) / 1000;
}

void dw_set_capture(dw_server *s, dw_capture_fn fn, void *ctx) {
    s->capture = fn;
    s->capture_ctx = ctx;
}
