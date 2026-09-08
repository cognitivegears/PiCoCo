#include "console.h"
#include "mode.h"
#include "bus.h"
#include "rom.h"
#include "becker.h"
#include "log.h"
#include "plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef PICOCO_VERSION
#define PICOCO_VERSION "dev"
#endif

static console_out_fn g_out;
static void *g_out_ctx;
static dw_server *g_dw;
static dw_store *g_store;

/* Remembered for "save" (rom_cmd empty = off, the default, so omitted). */
static char rom_cmd[64];

/* "dw capture" state. */
static dw_file cap_file;
static bool cap_open;
static uint32_t cap_off;

/* console_feed's line accumulator. */
static char linebuf[128];
static size_t linelen;

/* Scratch for "trace dump"; too big for a stack frame. */
static bus_trace_entry trace_buf[BUS_TRACE_SIZE];

static void outf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_out(g_out_ctx, buf);
}

static int cerr(const char *msg) {
    outf("err %s\n", msg);
    return -1;
}

static void fs_ls_cb(const char *name, uint32_t size, void *ctx) {
    (void)ctx;
    outf("%s %u\n", name, (unsigned)size);
}

static void console_capture(void *ctx, int dir, const uint8_t *buf, size_t n) {
    (void)ctx;
    uint8_t hdr[3] = { (uint8_t)dir, (uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
    g_store->ops->write(&cap_file, cap_off, hdr, sizeof(hdr));
    cap_off += sizeof(hdr);
    g_store->ops->write(&cap_file, cap_off, buf, (uint32_t)n);
    cap_off += (uint32_t)n;
}

static void capture_close(void) {
    if (!cap_open) return;
    g_store->ops->sync(&cap_file);
    g_store->ops->close(&cap_file);
    cap_open = false;
}

/* "dw selftest": drives dw_feed() directly against the live store on drive 3
 * (scratch), so it exercises READ/WRITE/READEX end to end with no CoCo and
 * no USB host attached. ponytail: clobbers whatever is mounted on drive 3;
 * fine for a diagnostic command run standalone. */
static uint8_t st_buf[300];
static size_t st_len;
static void selftest_send(void *ctx, const uint8_t *buf, size_t n) {
    (void)ctx;
    size_t cp = n > sizeof(st_buf) ? sizeof(st_buf) : n;
    memcpy(st_buf, buf, cp);
    st_len = cp;
}

static int cmd_dw_selftest(void) {
    if (!g_store->ops->create) { outf("selftest FAIL create\n"); return -1; }

    dw_file f;
    if (g_store->ops->create(g_store->ctx, "selftest.dsk", &f) != 0) {
        outf("selftest FAIL create\n");
        return -1;
    }
    static uint8_t sec[256]; /* static: core0 stack */
    bool write_ok = true;
    for (int n = 0; n < 10 && write_ok; n++) {
        memset(sec, 0, sizeof(sec));
        sec[0] = (uint8_t)n;
        if (g_store->ops->write(&f, (uint32_t)n * 256, sec, 256) != 256) write_ok = false;
    }
    g_store->ops->sync(&f);
    g_store->ops->close(&f);
    if (!write_ok) {
        plat_fs_remove("selftest.dsk");
        outf("selftest FAIL create\n");
        return -1;
    }

    if (dw_mount(g_dw, 3, "selftest.dsk", false) != 0) {
        plat_fs_remove("selftest.dsk");
        outf("selftest FAIL mount\n");
        return -1;
    }

    dw_send_fn old_send = g_dw->send;
    void *old_send_ctx = g_dw->send_ctx;
    bool old_hdbdos = g_dw->hdbdos;
    g_dw->send = selftest_send;
    g_dw->send_ctx = NULL;
    g_dw->hdbdos = false; /* direct drive/lsn addressing, not the HDB-DOS split */

    uint32_t now = plat_now_ms();
    const char *fail = NULL;

    /* READ drive 3 LSN 5: expect rc 0, data[0] == 5. */
    uint8_t read_req[5] = { DW_OP_READ, 3, 0, 0, 5 };
    st_len = 0;
    dw_feed(g_dw, read_req, sizeof(read_req), now);
    if (st_len != 259 || st_buf[0] != DW_E_OK || st_buf[3] != 5) fail = "read";

    /* WRITE drive 3 LSN 7 with a 0x5A fill: expect rc 0. */
    if (!fail) {
        static uint8_t data[256]; /* static: core0 stack */
        memset(data, 0x5A, sizeof(data));
        uint16_t sum = dw_checksum(data, sizeof(data));
        static uint8_t write_req[1 + 4 + 256 + 2]; /* static: core0 stack */
        write_req[0] = DW_OP_WRITE;
        write_req[1] = 3; write_req[2] = 0; write_req[3] = 0; write_req[4] = 7;
        memcpy(write_req + 5, data, sizeof(data));
        write_req[261] = (uint8_t)(sum >> 8);
        write_req[262] = (uint8_t)sum;
        st_len = 0;
        dw_feed(g_dw, write_req, sizeof(write_req), now);
        if (st_len != 1 || st_buf[0] != DW_E_OK) fail = "write";
    }

    /* READEX drive 3 LSN 7 + client checksum: expect data all 0x5A, then rc 0. */
    if (!fail) {
        uint8_t readex_req[5] = { DW_OP_READEX, 3, 0, 0, 7 };
        st_len = 0;
        dw_feed(g_dw, readex_req, sizeof(readex_req), now);
        bool data_ok = st_len == 256;
        for (size_t i = 0; data_ok && i < 256; i++) if (st_buf[i] != 0x5A) data_ok = false;
        if (!data_ok) {
            fail = "readex";
        } else {
            uint16_t sum = dw_checksum(st_buf, 256);
            uint8_t cksum[2] = { (uint8_t)(sum >> 8), (uint8_t)sum };
            st_len = 0;
            dw_feed(g_dw, cksum, sizeof(cksum), now);
            if (st_len != 1 || st_buf[0] != DW_E_OK) fail = "readex";
        }
    }

    g_dw->send = old_send;
    g_dw->send_ctx = old_send_ctx;
    g_dw->hdbdos = old_hdbdos;
    dw_eject(g_dw, 3);
    plat_fs_remove("selftest.dsk");

    if (fail) { outf("selftest FAIL %s\n", fail); return -1; }
    outf("selftest ok\n");
    return 0;
}

static int cmd_status(void) {
    outf("mode %s\n", mode_name(mode_get()));
    outf("mode reply_overflow %u\n", mode_stats.reply_overflow);
    outf("uptime_ms %u\n", plat_now_ms());
    outf("bus cycles %u reads %u writes %u write_overrun %u\n",
         bus_stats.cycles, bus_stats.reads, bus_stats.writes, bus_stats.write_overrun);
    outf("becker reads %u writes %u underrun %u overrun %u\n",
         becker_stats.reads, becker_stats.writes, becker_stats.underrun, becker_stats.overrun);
    outf("log_dropped %u\n", log_dropped);
    if (plat_usb_ejected()) outf("usb ejected\n");
    for (int i = 0; i < DW_MAX_DRIVES; i++) {
        if (g_dw->drives[i].mounted) {
            outf("drive %d %s%s\n", i, g_dw->drives[i].name,
                 g_dw->drives[i].read_only ? " ro" : "");
        }
    }
    return 0;
}

static int cmd_trace(int argc, char **argv) {
    if (argc < 2) return cerr("usage: trace dump [n]|freeze|run");
    if (strcasecmp(argv[1], "freeze") == 0) { bus_trace_freeze(true); return 0; }
    if (strcasecmp(argv[1], "run") == 0) { bus_trace_freeze(false); return 0; }
    if (strcasecmp(argv[1], "dump") == 0) {
        int n = argc >= 3 ? atoi(argv[2]) : 64;
        if (n < 1) n = 1;
        if (n > BUS_TRACE_SIZE) n = BUS_TRACE_SIZE;
        bus_trace_freeze(true);
        size_t got = bus_trace_copy(trace_buf, (size_t)n);
        for (size_t i = 0; i < got; i++) {
            outf("%u %04x %c %02x\n", trace_buf[i].t_us, trace_buf[i].idx,
                 trace_buf[i].rw ? 'R' : 'W', trace_buf[i].data);
        }
        bus_trace_freeze(false);
        return 0;
    }
    return cerr("usage: trace dump [n]|freeze|run");
}

static int cmd_rom(int argc, char **argv) {
    if (argc < 2) return cerr("usage: rom pattern|off|load <file>");
    if (strcasecmp(argv[1], "pattern") == 0) {
        rom_pattern();
        snprintf(rom_cmd, sizeof(rom_cmd), "pattern");
        return 0;
    }
    if (strcasecmp(argv[1], "off") == 0) {
        rom_off();
        rom_cmd[0] = '\0';
        return 0;
    }
    if (strcasecmp(argv[1], "load") == 0) {
        if (argc < 3) return cerr("usage: rom load <file>");
        int r = rom_load_file(g_store, argv[2]);
        if (r != 0) return cerr("rom load failed");
        snprintf(rom_cmd, sizeof(rom_cmd), "load %s", argv[2]);
        return 0;
    }
    return cerr("usage: rom pattern|off|load <file>");
}

static int cmd_becker(int argc, char **argv) {
    if (argc < 2) return cerr("usage: becker off|loop|bridge|native");
    picoco_mode m;
    if (strcasecmp(argv[1], "off") == 0) m = MODE_DIAG;
    else if (strcasecmp(argv[1], "loop") == 0) m = MODE_LOOP;
    else if (strcasecmp(argv[1], "bridge") == 0) m = MODE_BRIDGE;
    else if (strcasecmp(argv[1], "native") == 0) m = MODE_NATIVE;
    else return cerr("usage: becker off|loop|bridge|native");
    mode_set(m);
    return 0;
}

static int cmd_dw(int argc, char **argv) {
    if (argc < 2) return cerr("usage: dw mount|eject|hdbdos|stats|capture|selftest ...");
    if (strcasecmp(argv[1], "selftest") == 0) return cmd_dw_selftest();
    if (strcasecmp(argv[1], "mount") == 0) {
        if (argc < 4) return cerr("usage: dw mount <n> <file> [ro]");
        int n = atoi(argv[2]);
        bool ro = argc >= 5 && strcasecmp(argv[4], "ro") == 0;
        if (dw_mount(g_dw, n, argv[3], ro) != 0) return cerr("mount failed");
        return 0;
    }
    if (strcasecmp(argv[1], "eject") == 0) {
        if (argc < 3) return cerr("usage: dw eject <n>");
        int n = atoi(argv[2]);
        if (n < 0 || n >= DW_MAX_DRIVES) return cerr("bad drive");
        dw_eject(g_dw, n);
        return 0;
    }
    if (strcasecmp(argv[1], "hdbdos") == 0) {
        if (argc < 3) return cerr("usage: dw hdbdos on|off");
        if (strcasecmp(argv[2], "on") == 0) g_dw->hdbdos = true;
        else if (strcasecmp(argv[2], "off") == 0) g_dw->hdbdos = false;
        else return cerr("usage: dw hdbdos on|off");
        return 0;
    }
    if (strcasecmp(argv[1], "stats") == 0) {
        dw_stats *st = &g_dw->stats;
        outf("dw reads %u writes %u read_err %u write_err %u crc_err %u timeouts %u unknown_op %u notrdy %u\n",
             st->reads, st->writes, st->read_err, st->write_err, st->crc_err,
             st->timeouts, st->unknown_op, st->notrdy);
        for (int i = 0; i < 256; i++) if (st->ops[i]) outf("op %02x %u\n", i, st->ops[i]);
        return 0;
    }
    if (strcasecmp(argv[1], "capture") == 0) {
        if (argc < 3) return cerr("usage: dw capture on <file>|off");
        if (strcasecmp(argv[2], "on") == 0) {
            if (argc < 4) return cerr("usage: dw capture on <file>");
            if (!g_store->ops->create) return cerr("capture unsupported");
            capture_close();
            if (g_store->ops->create(g_store->ctx, argv[3], &cap_file) != 0) {
                return cerr("capture open failed");
            }
            cap_open = true;
            cap_off = 0;
            dw_set_capture(g_dw, console_capture, NULL);
            return 0;
        }
        if (strcasecmp(argv[2], "off") == 0) {
            dw_set_capture(g_dw, NULL, NULL);
            capture_close();
            return 0;
        }
        return cerr("usage: dw capture on <file>|off");
    }
    return cerr("usage: dw mount|eject|hdbdos|stats|capture|selftest ...");
}

static int cmd_fs(int argc, char **argv) {
    if (argc < 2) return cerr("usage: fs ls|rm|format|export|import");
    if (strcasecmp(argv[1], "ls") == 0) { plat_fs_list(fs_ls_cb, NULL); return 0; }
    if (strcasecmp(argv[1], "rm") == 0) {
        if (argc < 3) return cerr("usage: fs rm <file>");
        if (plat_fs_remove(argv[2]) != 0) return cerr("rm failed");
        return 0;
    }
    if (strcasecmp(argv[1], "format") == 0) {
        for (int i = 0; i < DW_MAX_DRIVES; i++) {
            if (g_dw->drives[i].mounted) return cerr("eject all drives first");
        }
        if (plat_fs_format() != 0) return cerr("format failed");
        return 0;
    }
    if (strcasecmp(argv[1], "export") == 0) {
        for (int i = 0; i < DW_MAX_DRIVES; i++) {
            if (g_dw->drives[i].mounted) return cerr("eject all drives first");
        }
        if (plat_fs_export(true) != 0) return cerr("export unsupported");
        outf("usb drive exported; run fs import or reboot when done\n");
        return 0;
    }
    if (strcasecmp(argv[1], "import") == 0) {
        if (plat_fs_export(false) != 0) return cerr("import unsupported");
        return 0;
    }
    return cerr("usage: fs ls|rm|format|export|import");
}

static int cmd_time(int argc, char **argv) {
    if (argc >= 2 && strcasecmp(argv[1], "set") == 0) {
        if (argc < 3) return cerr("usage: time set <unix>");
        dw_time_set(g_dw, atoll(argv[2]), plat_now_ms());
        return 0;
    }
    outf("time %lld\n", (long long)dw_time_get(g_dw, plat_now_ms()));
    return 0;
}

static int cmd_log(int argc, char **argv) {
    if (argc < 2) return cerr("usage: log <module> off|error|info|debug | log dump");
    if (strcasecmp(argv[1], "dump") == 0) {
        static char buf[4096]; /* static: Pico core0 stack is 4 KB */
        size_t n = log_drain(buf, sizeof(buf) - 1);
        buf[n] = '\0';
        g_out(g_out_ctx, buf);
        return 0;
    }
    if (argc < 3) return cerr("usage: log <module> off|error|info|debug");
    int m = log_module_by_name(argv[1]);
    if (m < 0) return cerr("no such module");
    int lvl;
    if (strcasecmp(argv[2], "off") == 0) lvl = LOG_OFF;
    else if (strcasecmp(argv[2], "error") == 0) lvl = LOG_ERROR;
    else if (strcasecmp(argv[2], "info") == 0) lvl = LOG_INFO;
    else if (strcasecmp(argv[2], "debug") == 0) lvl = LOG_DEBUG;
    else return cerr("usage: log <module> off|error|info|debug");
    log_set_level(m, lvl);
    return 0;
}

static int cmd_stats(int argc, char **argv) {
    if (argc < 2 || strcasecmp(argv[1], "reset") != 0) return cerr("usage: stats reset");
    bus_stats.cycles = 0; bus_stats.reads = 0; bus_stats.writes = 0; bus_stats.write_overrun = 0;
    becker_stats = (becker_stats_t){ 0 };
    memset(&g_dw->stats, 0, sizeof(g_dw->stats));
    mode_stats.reply_overflow = 0;
    return 0;
}

static bool cfg_append(char *cfg, size_t cap, size_t *len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(cfg + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) return false;
    *len += (size_t)n;
    return true;
}

static int cmd_save(void) {
    static char cfg[1024]; /* static: Pico core0 stack is 4 KB */
    size_t len = 0;
    if (!cfg_append(cfg, sizeof(cfg), &len, "becker %s\n", mode_name(mode_get())))
        return cerr("config too large");
    if (rom_cmd[0] && !cfg_append(cfg, sizeof(cfg), &len, "rom %s\n", rom_cmd))
        return cerr("config too large");
    if (!cfg_append(cfg, sizeof(cfg), &len, "dw hdbdos %s\n", g_dw->hdbdos ? "on" : "off"))
        return cerr("config too large");
    for (int i = 0; i < DW_MAX_DRIVES; i++) {
        if (g_dw->drives[i].mounted) {
            if (!cfg_append(cfg, sizeof(cfg), &len, "dw mount %d %s%s\n", i,
                             g_dw->drives[i].name, g_dw->drives[i].read_only ? " ro" : ""))
                return cerr("config too large");
        }
    }
    for (int m = 0; m < LOG_M_COUNT; m++) {
        int lvl = log_level(m);
        if (lvl == LOG_INFO) continue;
        const char *lvlname = lvl == LOG_OFF ? "off" : lvl == LOG_ERROR ? "error" : "debug";
        if (!cfg_append(cfg, sizeof(cfg), &len, "log %s %s\n", log_module_names[m], lvlname))
            return cerr("config too large");
    }
    if (plat_cfg_write(cfg, len) != 0) return cerr("cfg write failed");
    return 0;
}

/* ponytail: linear if-chain dispatch; table when >30 commands. */
static int dispatch(int argc, char **argv) {
    const char *v = argv[0];
    if (strcasecmp(v, "help") == 0) {
        outf("commands: help status version smoke halt trace rom becker dw fs time log stats save reboot bootsel\n");
        return 0;
    }
    if (strcasecmp(v, "status") == 0) return cmd_status();
    if (strcasecmp(v, "version") == 0) { outf("version %s\n", PICOCO_VERSION); return 0; }
    if (strcasecmp(v, "smoke") == 0) { plat_smoke(); return 0; }
    if (strcasecmp(v, "halt") == 0) {
        if (argc < 2) return cerr("usage: halt on|off");
        if (strcasecmp(argv[1], "on") == 0) { plat_halt(true); return 0; }
        if (strcasecmp(argv[1], "off") == 0) { plat_halt(false); return 0; }
        return cerr("usage: halt on|off");
    }
    if (strcasecmp(v, "trace") == 0) return cmd_trace(argc, argv);
    if (strcasecmp(v, "rom") == 0) return cmd_rom(argc, argv);
    if (strcasecmp(v, "becker") == 0) return cmd_becker(argc, argv);
    if (strcasecmp(v, "dw") == 0) return cmd_dw(argc, argv);
    if (strcasecmp(v, "fs") == 0) return cmd_fs(argc, argv);
    if (strcasecmp(v, "time") == 0) return cmd_time(argc, argv);
    if (strcasecmp(v, "log") == 0) return cmd_log(argc, argv);
    if (strcasecmp(v, "stats") == 0) return cmd_stats(argc, argv);
    if (strcasecmp(v, "save") == 0) return cmd_save();
    if (strcasecmp(v, "reboot") == 0) { plat_reboot(false); return 0; }
    if (strcasecmp(v, "bootsel") == 0) { plat_reboot(true); return 0; }
    return cerr("unknown command");
}

void console_init(console_out_fn out, void *ctx, dw_server *dw, dw_store *store) {
    g_out = out;
    g_out_ctx = ctx;
    g_dw = dw;
    g_store = store;
    linelen = 0;
    rom_cmd[0] = '\0';
    cap_open = false;
    cap_off = 0;
    mode_bind(dw);
}

int console_exec(const char *line) {
    char copy[136];
    snprintf(copy, sizeof(copy), "%s", line);
    char *save = NULL;
    char *argv[6];
    int argc = 0;
    char *tok = strtok_r(copy, " ", &save);
    while (tok && argc < 6) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " ", &save);
    }
    if (argc == 0) return cerr("empty");
    int rc = dispatch(argc, argv);
    if (rc == 0) outf("ok\n");
    return rc;
}

void console_feed(const uint8_t *buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            if (linelen > 0) {
                linebuf[linelen] = '\0';
                console_exec(linebuf);
                linelen = 0;
            }
        } else if (linelen < sizeof(linebuf) - 1) {
            linebuf[linelen++] = c;
        }
    }
}

int console_run_config(void) {
    static char buf[1024]; /* static: core0 stack */
    int n = plat_cfg_read(buf, sizeof(buf) - 1);
    if (n < 0) return -1;
    buf[n] = '\0';
    int count = 0;
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\r') line[l - 1] = '\0';
        if (line[0] != '\0' && line[0] != '#') {
            if (console_exec(line) != 0) LOG_E(LOG_M_CONSOLE, "config: %s", line);
            count++;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    return count;
}
