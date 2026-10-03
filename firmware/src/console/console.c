#include "console.h"
#include "mode.h"
#include "bus.h"
#include "bus_engine.h"
#include "rom.h"
#include "becker.h"
#include "log.h"
#include "plat.h"
#include "dw_disk.h"
#include "net.h"
#include "ui.h"
#include "manager_rom.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifndef PICOCO_HOST
#include "pico/platform/panic.h"
#endif
#ifdef PICOCO_BOARD_H
#include PICOCO_BOARD_H
#endif

#ifndef PICOCO_VERSION
#define PICOCO_VERSION "dev"
#endif

#ifdef PICOCO_HAVE_FAKE6809
#include "fake6809.h"
#endif

static console_out_fn g_out;
static void *g_out_ctx;
static dw_server *g_dw;
static dw_store *g_store;

/* Remembered for "save" (rom_cmd empty = no choice saved, so the built-in manager loads at boot; "off" is stored explicitly). */
static char rom_cmd[64];
static char rom_now[64];   /* what is in bus_table now */

/* "dw capture" state. */
static dw_file cap_file;
static bool cap_open;
static uint32_t cap_off;

/* console_feed's line accumulator. */
static char linebuf[128];
static size_t linelen;

/* Scratch for "trace dump"; too big for a stack frame. */
static bus_trace_entry trace_buf[BUS_TRACE_SIZE];

/* The line being executed, untokenised: "dw disk insert" re-joins its tail
 * so image names may contain spaces (DW4 behaviour). */
static const char *g_raw = "";

static const char *raw_tail(int skip) {
    static char tail[128];
    const char *p = g_raw;
    for (int i = 0; i < skip; i++) {
        while (*p == ' ') p++;
        while (*p && *p != ' ') p++;
    }
    while (*p == ' ') p++;
    snprintf(tail, sizeof(tail), "%s", p);
    size_t n = strlen(tail);
    while (n && (tail[n - 1] == ' ' || tail[n - 1] == '\r' || tail[n - 1] == '\n')) tail[--n] = '\0';
    return tail;
}

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

#ifndef PICOCO_HOST
static void selftest_line(const char *s) { outf("%s\n", s); }
#endif

static int cmd_bus(int argc, char **argv) {
    if (argc < 2) { outf("bus drive %s\n", bus_drive_get() ? "on" : "off"); return 0; }
    if (strcasecmp(argv[1], "drive") == 0) {
        if (argc < 3) return cerr("usage: bus drive on|off");
        if (strcasecmp(argv[2], "on") == 0) { bus_drive_set(true); return 0; }
        if (strcasecmp(argv[2], "off") == 0) { bus_drive_set(false); return 0; }
        return cerr("usage: bus drive on|off");
    }
    if (strcasecmp(argv[1], "selftest") == 0) {
        if (argc >= 3 && strcasecmp(argv[2], "net") == 0) {
#ifdef PICOCO_HAVE_NET
            return cerr("bus selftest net: not supported by this engine");
#else
            return cerr("net: needs Plus-W");
#endif
        }
#ifndef PICOCO_HAVE_FAKE6809
        return cerr("bus selftest: pico only (host build)");
#else
        int opts = 0;
        for (int i = 2; i < argc; i++) {
            if (strcasecmp(argv[i], "stress") == 0) opts |= FAST_OPT_STRESS;
            else if (strcasecmp(argv[i], "radio") == 0) opts |= FAST_OPT_RADIO;
            else if (strcasecmp(argv[i], "restarts") == 0) opts |= FAST_OPT_RESTARTS;
            else if (strcasecmp(argv[i], "switches") == 0) opts |= FAST_OPT_SWITCHES;
            else return cerr("usage: bus selftest [stress] [radio] [restarts] [switches] | bus selftest net");
        }
        int rc = fake6809_fast(opts, selftest_line);
        if (rc == -2) return cerr("bus selftest: bus is live (CoCo attached), refused");
        if (rc != 0) return cerr("selftest fast FAIL");
        outf("selftest fast pass\n");
        return 0;
#endif
    }
#ifndef PICOCO_HOST
    if (strcasecmp(argv[1], "engine") == 0) {   /* who holds which PIO SM and DMA channel */
        bus_engine_resources(selftest_line);
        return 0;
    }
#endif
    return cerr("usage: bus drive on|off | bus selftest [stress] [radio] [restarts] [switches] | bus selftest net | bus engine");
}

/* "crash panic" is a hidden subcommand (not in help): exercises the
 * PICO_PANIC_FUNCTION path via panic() instead of the hardfault path. */
static int cmd_crash(int argc, char **argv) {
#ifdef PICOCO_HOST
    (void)argc; (void)argv;
    return cerr("crash test is Pico only");
#else
    if (argc >= 2 && strcasecmp(argv[1], "panic") == 0) {
        outf("crashing now\n");
        panic("console");
        return 0;   /* unreachable: panic() never returns */
    }
    outf("crashing now\n");
    plat_crash_test();
    return 0;   /* unreachable on Pico: plat_crash_test() never returns */
#endif
}

static int cmd_status(void) {
    outf("mode %s\n", mode_name(mode_get()));
    outf("mode reply_overflow %u\n", mode_stats.reply_overflow);
    outf("uptime_ms %u\n", plat_now_ms());
    outf("bus cycles %u reads %u writes %u write_overrun %u\n",
         bus_stats.cycles, bus_stats.reads, bus_stats.writes, bus_stats.write_overrun);
    outf("bus engine_stall %u event_lag_max %u event_drop %u event_lap %u start_wait_cap %u\n", bus_stats.engine_stall, bus_stats.event_lag_max, bus_stats.event_drop, bus_stats.event_lap, bus_stats.start_wait_cap);
    outf("bus whooks %u\n", bus_stats.whooks_run);
    outf("bus drive %s\n", bus_drive_get() ? "on" : "off");
    outf("last reset %s\n", plat_last_reset());
    outf("dw hdbdos %s\n", g_dw->hdbdos ? "on" : "off"); /* DWINIT can flip this remotely */
    outf("rom now %s\n", rom_now[0] ? rom_now : "none");
    outf("rom next %s\n", rom_cmd[0] ? rom_cmd : "none");
    outf("becker reads %u writes %u underrun %u overrun %u\n",
         becker_stats.reads, becker_stats.writes, becker_stats.underrun, becker_stats.overrun);
    if (net_available()) outf("net %s %s\n", net_state_name(net_state()), net_ip());
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
        bool was_frozen = bus_trace_frozen();   /* frozen by a fault or `trace freeze`: stays frozen until `trace run` */
        bus_trace_freeze(true);
        size_t got = bus_trace_copy(trace_buf, (size_t)n);
        for (size_t i = 0; i < got; i++) {
            outf("%u %04x %c %02x\n", (unsigned)trace_buf[i].seq, trace_buf[i].idx,
                 trace_buf[i].rw ? 'R' : 'W', trace_buf[i].data);
        }
        if (!was_frozen) bus_trace_thaw();     /* keeps the start: a second dump on an idle bus shows the same events */
        return 0;
    }
    return cerr("usage: trace dump [n]|freeze|run");
}

void console_boot_manager(void) {
    if (rom_load_mem(manager_rom, manager_rom_len) == 0) snprintf(rom_now, sizeof(rom_now), "load manager");
}

/* Spec 2026-10-01 s6.5: a board with no ROM choice saved boots the manager.
 * `rom off` is a choice; an absent line is not. */
static void rom_fallback(void) {
    if (rom_cmd[0] || rom_loaded()) return;
    if (rom_load_mem(manager_rom, manager_rom_len) == 0) snprintf(rom_now, sizeof(rom_now), "load manager");
}

static int cmd_rom(int argc, char **argv) {
    if (argc < 2) return cerr("usage: rom pattern|off|load <file>|launch <file>|boot <file>");
    if (plat_fs_exporting() &&
        (strcasecmp(argv[1], "load") == 0 || strcasecmp(argv[1], "launch") == 0 ||
         strcasecmp(argv[1], "boot") == 0))
        return cerr("fs export active; run fs import first");
    if (strcasecmp(argv[1], "pattern") == 0) {
        rom_pattern();
        snprintf(rom_cmd, sizeof(rom_cmd), "pattern");
        snprintf(rom_now, sizeof(rom_now), "pattern");
        return 0;
    }
    if (strcasecmp(argv[1], "off") == 0) {
        rom_off();
        snprintf(rom_cmd, sizeof(rom_cmd), "off");
        rom_now[0] = '\0';
        return 0;
    }
    /* launch = load now, but leave the saved next-boot choice alone. */
    bool launch = strcasecmp(argv[1], "launch") == 0;
    if (launch || strcasecmp(argv[1], "load") == 0) {
        if (argc < 3) return cerr("usage: rom load|launch <file>");
        /* "manager" is the built-in stub (src/ui/manager_rom.h), not a file. */
        int r = strcasecmp(argv[2], "manager") == 0
              ? rom_load_mem(manager_rom, manager_rom_len)
              : rom_load_file(g_store, argv[2]);
        if (r == -2) return cerr("rom load: size must be 8K, 16K, or banked 32K/64K/128K");
        if (r != 0) return cerr("rom load failed");
        if (!launch) snprintf(rom_cmd, sizeof(rom_cmd), "load %s", argv[2]);
        snprintf(rom_now, sizeof(rom_now), "load %s", argv[2]);
        return 0;
    }
    if (strcasecmp(argv[1], "boot") == 0) {
        /* Next boot only: a live swap would change the DOS under a running CoCo. */
        if (argc < 3) return cerr("usage: rom boot <file>");
        int r = strcasecmp(argv[2], "manager") == 0 ? 0 : rom_check_file(g_store, argv[2]);
        if (r == -2) return cerr("rom must be 8192 or 16384 bytes");
        if (r != 0) return cerr("rom not found");
        snprintf(rom_cmd, sizeof(rom_cmd), "load %s", argv[2]);
        return 0;
    }
    return cerr("usage: rom pattern|off|load <file>|launch <file>|boot <file>");
}

static int g_boot_mode = -1; /* pending next-boot mode from "net mode", -1 = none */
void console_set_boot_mode(picoco_mode m) { g_boot_mode = (int)m; }

static int cmd_becker(int argc, char **argv) {
    if (argc < 2) return cerr("usage: becker off|loop|bridge|native|net");
    picoco_mode m;
    if (strcasecmp(argv[1], "off") == 0) m = MODE_DIAG;
    else if (strcasecmp(argv[1], "loop") == 0) m = MODE_LOOP;
    else if (strcasecmp(argv[1], "bridge") == 0) m = MODE_BRIDGE;
    else if (strcasecmp(argv[1], "native") == 0) m = MODE_NATIVE;
    else if (strcasecmp(argv[1], "net") == 0) {
        if (!net_available()) return cerr("net: needs Plus-W");
        if (!net_configured()) return cerr("net: set ssid and server first");
        if (net_start() != 0) {   /* keep the saved intent, run native meanwhile */
            if (mode_get() == MODE_NET) net_stop();
            mode_set(MODE_NATIVE);
            console_set_boot_mode(MODE_NET);
            outf("net: start failed, running native\n");
            return 0;
        }
        m = MODE_NET;
    }
    else return cerr("usage: becker off|loop|bridge|native|net");
    if (mode_get() == MODE_NET && m != MODE_NET) net_stop();
    mode_set(m);
    g_boot_mode = -1;
    return 0;
}

static void scan_line(const char *ssid, int rssi, int chan, void *ctx) {
    (void)ctx;
    outf("ssid %s rssi %d chan %d\n", ssid, rssi, chan);
}
/* Drop out of net mode before touching its config so a live socket is
 * never reconfigured underneath the mode pump. */
static void net_leave_if_active(void) {
    if (mode_get() == MODE_NET) { net_stop(); mode_set(MODE_NATIVE); }
}
static int cmd_net(int argc, char **argv) {
    const char *usage = "usage: net [status]|tz <minutes>|off|join <ssid>|psk [<psk>]|server <host> [port]|forget|scan|mode net|native";
    if (argc < 2 || strcasecmp(argv[1], "status") == 0) {
        outf("net state %s\n", net_state_name(net_state()));
        if (net_last_error()[0]) outf("net error %s\n", net_last_error());
        outf("net ssid %s\n", net_ssid());
        outf("net psk %s\n", net_psk_set() ? "set" : "unset");
        outf("net server %s %u\n", net_host(), net_port());
        outf("net ip %s\n", net_ip());
        outf("net bytes up %u down %u overrun %u retries %u\n",
             net_stats.bytes_up, net_stats.bytes_down, net_stats.overrun, net_stats.retries);
        if (net_tz() == NET_TZ_OFF) outf("net tz off\n"); else outf("net tz %d\n", net_tz());
        outf("net radio %s\n", net_available() ? "yes" : "no");
        outf("net mode %s boot %s\n", mode_name(mode_get()),
             mode_name(g_boot_mode >= 0 ? (picoco_mode)g_boot_mode : mode_get()));
        return 0;
    }
    if (strcasecmp(argv[1], "join") == 0) {
        if (argc < 3) return cerr(usage);
        net_leave_if_active();
        return net_set_ssid(raw_tail(2)) == 0 ? 0 : cerr("net: ssid too long");
    }
    if (strcasecmp(argv[1], "psk") == 0) {
        net_leave_if_active();
        return net_set_psk(argc < 3 ? "" : raw_tail(2)) == 0 ? 0 : cerr("net: psk too long");
    }
    if (strcasecmp(argv[1], "server") == 0) {
        if (argc < 3) return cerr(usage);
        long port = argc >= 4 ? atol(argv[3]) : NET_DEFAULT_PORT;
        if (port < 1 || port > 65535) return cerr("net: bad port");
        net_leave_if_active();
        return net_set_server(argv[2], (uint16_t)port) == 0 ? 0 : cerr("net: host too long");
    }
    if (strcasecmp(argv[1], "tz") == 0) {
        char *end; long m = 0; int off = 0;
        if (argc < 3) return cerr(usage);
        if (strcasecmp(argv[2], "off") == 0) off = 1;
        else { m = strtol(argv[2], &end, 10); if (*end || end == argv[2]) return cerr("net: tz is minutes or off"); }
        return (off || (m >= -840 && m <= 840)) && net_set_tz(off ? NET_TZ_OFF : (int)m) == 0 ? 0 : cerr("net: tz out of range (+-840)");
    }
    if (strcasecmp(argv[1], "forget") == 0) { net_leave_if_active(); net_forget(); g_boot_mode = -1; return 0; }
    if (strcasecmp(argv[1], "scan") == 0) {
        if (mode_get() == MODE_NET) return cerr("net: leave net mode first (becker native)");
        if (!net_available()) return cerr("net: needs Plus-W");
        if (net_scan(scan_line, NULL) != 0) return cerr("net: scan failed");
        return 0;
    }
    if (strcasecmp(argv[1], "mode") == 0) {
        if (argc < 3) return cerr(usage);
        /* Next-boot mode only: switching the running mode would route the
         * manager's own channel to the server and drop its reply. */
        if (strcasecmp(argv[2], "net") == 0) {
            if (!net_available()) return cerr("net: needs Plus-W");
            if (!net_configured()) return cerr("net: set ssid and server first");
            g_boot_mode = MODE_NET;
            return 0;
        }
        if (strcasecmp(argv[2], "native") == 0) { g_boot_mode = MODE_NATIVE; return 0; }
        return cerr(usage);
    }
    return cerr(usage);
}

/* -1 unless s is a single digit naming a drive. */
static int parse_drive(const char *s) {
    if (!isdigit((unsigned char)s[0]) || s[1] != '\0') return -1;
    int n = s[0] - '0';
    return n < DW_MAX_DRIVES ? n : -1;
}

/* DW4-compatible "dw disk" (spec 2026-09-23 §4.4). Output formats follow DW4
 * DWCmdDiskShow/Insert/Eject so NitrOS-9's dw utility reads them unchanged. */
static int cmd_dw_disk(int argc, char **argv) {
    const char *usage = "usage: dw disk show [n]|insert <n> <file>|eject <n>";
    if (argc < 3) return cerr(usage);
    if (strcasecmp(argv[2], "show") == 0) {
        if (argc >= 4) {
            int n = parse_drive(argv[3]);
            if (n < 0) return cerr("bad drive");
            if (!g_dw->drives[n].mounted) return cerr("drive not loaded");
            outf("Details for disk in drive #%d:\r\n\r\n%s\r\n", n, g_dw->drives[n].name);
            return 0;
        }
        outf("\r\nCurrent DriveWire disks:\r\n\r\n");
        for (int i = 0; i < DW_MAX_DRIVES; i++) {
            dw_disk *d = &g_dw->drives[i];
            if (d->mounted) outf("X%-3d%c%s\r\n", i, d->read_only ? '*' : ' ', d->name);
        }
        return 0;
    }
    if (strcasecmp(argv[2], "insert") == 0) {
        if (argc < 5) return cerr(usage);
        int n = parse_drive(argv[3]);
        if (n < 0) return cerr("bad drive");
        int mr = dw_mount(g_dw, n, raw_tail(4), false);
        if (mr == -3) return cerr("already mounted");
        if (mr != 0) return cerr("mount failed");
        outf("Disk inserted in drive %d.", n);
        return 0;
    }
    if (strcasecmp(argv[2], "eject") == 0) {
        if (argc < 4) return cerr(usage);
        int n = parse_drive(argv[3]);
        if (n < 0) return cerr("bad drive");
        if (!g_dw->drives[n].mounted) return cerr("drive not loaded");
        dw_eject(g_dw, n);
        outf("Disk ejected from drive %d.\r\n", n);
        return 0;
    }
    return cerr(usage);
}

static int cmd_cart(int argc, char **argv) {
    if (argc < 2) { outf("cart %s\n", rom_cart_get() == CART_ON ? "on" : rom_cart_get() == CART_OFF ? "off" : "auto"); return 0; }
    if (strcasecmp(argv[1], "on") == 0) {
#if defined(PICOCO_BOARD_H) && !defined(PIN_CART_DRV)
        return cerr("cart: needs Plus-W (JP5 on a Pico 2)");
#else
        rom_cart_set(CART_ON);
        return 0;
#endif
    }
    if (strcasecmp(argv[1], "off") == 0) {
#if defined(PICOCO_BOARD_H) && !defined(PIN_CART_DRV)
        return cerr("cart: needs Plus-W (JP5 on a Pico 2)");
#else
        rom_cart_set(CART_OFF);
        return 0;
#endif
    }
    if (strcasecmp(argv[1], "auto") == 0) { rom_cart_set(CART_AUTO); return 0; }
    return cerr("usage: cart on|off|auto");
}

static int cmd_dw(int argc, char **argv) {
    if (argc < 2) return cerr("usage: dw mount|disk|eject|hdbdos|stats|capture|selftest ...");
    if (plat_fs_exporting()) return cerr("fs export active; run fs import first");
    if (strcasecmp(argv[1], "selftest") == 0) return cmd_dw_selftest();
    if (strcasecmp(argv[1], "disk") == 0) return cmd_dw_disk(argc, argv);
    if (strcasecmp(argv[1], "mount") == 0) {
        if (argc < 4) return cerr("usage: dw mount <n> <file> [ro]");
        int n = atoi(argv[2]);
        bool ro = argc >= 5 && strcasecmp(argv[4], "ro") == 0;
        int mr = dw_mount(g_dw, n, argv[3], ro);
        if (mr == -3) return cerr("already mounted");
        if (mr != 0) return cerr("mount failed");
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
    return cerr("usage: dw mount|disk|eject|hdbdos|stats|capture|selftest ...");
}

/* Shared by "fs format" and "fs export": both yank FatFS out from under
 * anything still using it. A mounted drive holds an open dw_disk FIL; an
 * open capture holds an open cap_file FIL. */
static const char *fs_busy_reason(void) {
    for (int i = 0; i < DW_MAX_DRIVES; i++) {
        if (g_dw->drives[i].mounted) return "eject all drives first";
    }
    if (cap_open) return "dw capture off first";
    return NULL;
}

#define RSDOS_35T_BYTES (35u * 18u * 256u)         /* 161280 */
#define RSDOS_DIR_OFF   (17u * 18u * 256u)         /* track 17 sector 1 = 0x13200 */
#define RSDOS_FAT_OFF   ((17u * 18u + 1u) * 256u)  /* track 17 sector 2 = 0x13300 */

/* A blank 35-track RS-DOS disk as real DSKINI leaves it (verified against
 * ToolShed's decb dskini): all $FF, except track 17 sector 1 (unused by
 * RS-DOS; the directory itself is sectors 3-11) is entirely $00, and the
 * FAT sector's (track 17 sector 2) bytes 68..255 are $00 (68 free
 * granules, the rest unused). */
static int cmd_fs_new(const char *name) {
    if (strlen(name) >= 32) return cerr("name too long"); /* dw_disk name[32] can't hold it */
    if (dw_disk_is_config_name(name)) return cerr("reserved name");
    if (!dw_disk_name_ok(name)) return cerr("bad name");
    dw_file f;
    if (g_store->ops->open(g_store->ctx, name, false, &f) >= 0) {
        g_store->ops->close(&f);
        return cerr("file exists");
    }
    if (!g_store->ops->create || g_store->ops->create(g_store->ctx, name, &f) != 0)
        return cerr("create failed");
    static uint8_t blk[4096]; /* static: Pico core0 stack is 4 KB */
    int rc = 0;
    for (uint32_t off = 0; off < RSDOS_35T_BYTES && rc == 0; off += sizeof(blk)) {
        uint32_t n = RSDOS_35T_BYTES - off < sizeof(blk) ? RSDOS_35T_BYTES - off : sizeof(blk);
        memset(blk, 0xFF, n);
        if (off <= RSDOS_DIR_OFF && RSDOS_DIR_OFF < off + n)
            memset(blk + (RSDOS_DIR_OFF - off), 0x00, 256);
        if (off <= RSDOS_FAT_OFF && RSDOS_FAT_OFF < off + n)
            memset(blk + (RSDOS_FAT_OFF - off) + 68, 0x00, 256 - 68);
        if (g_store->ops->write(&f, off, blk, n) != (int)n) rc = -1;
    }
    if (rc == 0 && g_store->ops->sync && g_store->ops->sync(&f) != 0) rc = -1;
    g_store->ops->close(&f);
    if (rc != 0) {
        plat_fs_remove(name);
        return cerr("write failed");
    }
    return 0;
}

static int cmd_fs(int argc, char **argv) {
    if (argc < 2) return cerr("usage: fs ls|new|rm|format|export|import");
    if (plat_fs_exporting() && strcasecmp(argv[1], "import") != 0)
        return cerr("fs export active; run fs import first");
    if (strcasecmp(argv[1], "ls") == 0) { plat_fs_list(fs_ls_cb, NULL); return 0; }
    if (strcasecmp(argv[1], "new") == 0) {
        if (argc < 3) return cerr("usage: fs new <file>");
        return cmd_fs_new(raw_tail(2)); /* "fs new" is 2 tokens; rest is the name, spaces and all */
    }
    if (strcasecmp(argv[1], "rm") == 0) {
        if (argc < 3) return cerr("usage: fs rm <file>");
        if (plat_fs_remove(argv[2]) != 0) return cerr("rm failed");
        return 0;
    }
    if (strcasecmp(argv[1], "format") == 0) {
        const char *busy = fs_busy_reason();
        if (busy) return cerr(busy);
        if (plat_fs_format() != 0) return cerr("format failed");
        return 0;
    }
    if (strcasecmp(argv[1], "export") == 0) {
        const char *busy = fs_busy_reason();
        if (busy) return cerr(busy);
        if (plat_fs_export(true) != 0) return cerr("export unsupported");
        outf("usb drive exported; run fs import or reboot when done\n");
        return 0;
    }
    if (strcasecmp(argv[1], "import") == 0) {
        if (!plat_usb_ejected()) return cerr("eject the PICOCO volume on the host first");
        if (plat_fs_export(false) != 0) return cerr("import unsupported");
        return 0;
    }
    return cerr("usage: fs ls|new|rm|format|export|import");
}

static int cmd_time(int argc, char **argv) {
    if (argc >= 2 && strcasecmp(argv[1], "set") == 0) {
        if (argc < 3) return cerr("usage: time set <unix>");
        int64_t t = atoll(argv[2]);
        dw_time_set(g_dw, t, plat_now_ms());
        plat_rtc_set(t);
        return 0;
    }
    int64_t dummy;
    outf("time %lld\n", (long long)dw_time_get(g_dw, plat_now_ms()));
    outf("clock %s\n", plat_rtc_get(&dummy) ? "kept" : "lost");
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
    bus_stats.whooks_run = 0; bus_stats.engine_stall = 0; bus_stats.event_lag_max = 0; bus_stats.event_drop = 0; bus_stats.event_lap = 0; bus_stats.start_wait_cap = 0;
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
    if (net_ssid()[0] && !cfg_append(cfg, sizeof(cfg), &len, "net join %s\n", net_ssid())) return cerr("config too large");
    if (net_psk_set() && !cfg_append(cfg, sizeof(cfg), &len, "net psk %s\n", net_psk_plain())) return cerr("config too large");
    if (net_host()[0] && !cfg_append(cfg, sizeof(cfg), &len, "net server %s %u\n", net_host(), net_port())) return cerr("config too large");
    if (net_tz() != NET_TZ_OFF && !cfg_append(cfg, sizeof(cfg), &len, "net tz %d\n", net_tz())) return cerr("config too large");
    if (!cfg_append(cfg, sizeof(cfg), &len, "becker %s\n",
                    mode_name(g_boot_mode >= 0 ? (picoco_mode)g_boot_mode : mode_get())))
        return cerr("config too large");
    if (rom_cmd[0] && !cfg_append(cfg, sizeof(cfg), &len, "rom %s\n", rom_cmd))
        return cerr("config too large");
    if (rom_cart_get() != CART_AUTO && !cfg_append(cfg, sizeof(cfg), &len, "cart %s\n", rom_cart_get() == CART_ON ? "on" : "off"))
        return cerr("config too large");
    if (bus_drive_get() && !cfg_append(cfg, sizeof(cfg), &len, "bus drive on\n"))
        return cerr("config too large");
    if (!cfg_append(cfg, sizeof(cfg), &len, "dw hdbdos %s\n", g_dw->hdbdos ? "on" : "off"))
        return cerr("config too large");
    for (int i = 0; i < DW_MAX_DRIVES; i++) {
        if (!g_dw->drives[i].mounted) continue;
        /* "dw mount" takes one token, so a read-write name with spaces (e.g.
         * "my disk.dsk") replays as "dw mount 0 my" and fails. "dw disk
         * insert" re-joins its raw tail (console_run_config sets g_raw via
         * console_exec), so use it for rw mounts; ro has no equivalent on
         * "dw disk", so keep "dw mount ... ro" there (ro names still can't
         * have spaces, unchanged from before). */
        bool ok = g_dw->drives[i].read_only
            ? cfg_append(cfg, sizeof(cfg), &len, "dw mount %d %s ro\n", i, g_dw->drives[i].name)
            : cfg_append(cfg, sizeof(cfg), &len, "dw disk insert %d %s\n", i, g_dw->drives[i].name);
        if (!ok) return cerr("config too large");
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
        outf("commands: help status version smoke halt trace rom becker net cart bus crash dw fs time log stats save reboot bootsel\n");
        return 0;
    }
    if (strcasecmp(v, "status") == 0) return cmd_status();
    if (strcasecmp(v, "version") == 0) { outf("version %s\n", PICOCO_VERSION); return 0; }
    if (strcasecmp(v, "smoke") == 0) {
        if (bus_drive_get()) return cerr("bus drive on");
        plat_smoke();
        return 0;
    }
    if (strcasecmp(v, "bus") == 0) return cmd_bus(argc, argv);
    if (strcasecmp(v, "crash") == 0) return cmd_crash(argc, argv);
    if (strcasecmp(v, "halt") == 0) {
        if (argc < 2) return cerr("usage: halt on|off");
        if (strcasecmp(argv[1], "on") == 0) { plat_halt(true); return 0; }
        if (strcasecmp(argv[1], "off") == 0) { plat_halt(false); return 0; }
        return cerr("usage: halt on|off");
    }
    if (strcasecmp(v, "trace") == 0) return cmd_trace(argc, argv);
    if (strcasecmp(v, "rom") == 0) return cmd_rom(argc, argv);
    if (strcasecmp(v, "becker") == 0) return cmd_becker(argc, argv);
    if (strcasecmp(v, "net") == 0) return cmd_net(argc, argv);
    if (strcasecmp(v, "cart") == 0) return cmd_cart(argc, argv);
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
    g_boot_mode = -1;
    rom_cmd[0] = '\0';
    rom_now[0] = '\0';
    cap_open = false;
    cap_off = 0;
    mode_bind(dw);
    ui_init(mode_dw_send, NULL, console_exec_capture, store);
    becker_set_ctl(ui_ctl);
    dw_set_exec(dw, console_exec_remote, NULL);
}

static int tokenize(char *copy, char **argv) {
    char *save = NULL;
    int argc = 0;
    char *tok = strtok_r(copy, " ", &save);
    while (tok && argc < 6) {
        argv[argc++] = tok;
        tok = strtok_r(NULL, " ", &save);
    }
    return argc;
}

int console_exec(const char *line) {
    char copy[136];
    snprintf(copy, sizeof(copy), "%s", line);
    char *argv[6];
    int argc = tokenize(copy, argv);
    if (argc == 0) return cerr("empty");
    g_raw = line;
    int rc = dispatch(argc, argv);
    if (rc == 0) outf("ok\n");
    return rc;
}

/* DriveWire virtual-serial front end (spec 2026-09-23 §4.3). Deny by
 * default: a new console command is USB-only until it is listed here. */
static const char *const remote_allow[] = {
    "status", "version", "help", "time", "save", "net",
    "fs ls", "fs new", "dw mount", "dw eject", "dw hdbdos", "dw disk", "rom boot",
};

static bool remote_allowed(int argc, char **argv) {
    for (size_t i = 0; i < sizeof(remote_allow) / sizeof(remote_allow[0]); i++) {
        const char *e = remote_allow[i];
        const char *sp = strchr(e, ' ');
        size_t vl = sp ? (size_t)(sp - e) : strlen(e);
        if (strlen(argv[0]) != vl || strncasecmp(argv[0], e, vl) != 0) continue;
        if (!sp) return true;
        if (argc >= 2 && strcasecmp(argv[1], sp + 1) == 0) return true;
    }
    return false;
}

static char *rcap;
static size_t rcap_cap, rcap_len;
static bool rcap_trunc;

static void remote_out(void *ctx, const char *s) {
    (void)ctx;
    size_t l = strlen(s);
    if (rcap_len + l > rcap_cap) {
        l = rcap_cap - rcap_len;
        rcap_trunc = true;
    }
    memcpy(rcap + rcap_len, s, l);
    rcap_len += l;
}

/* snprintf() returns the length it would have written, which can exceed
 * cap - 1 on truncation; outn must never claim more than fits in out. */
static size_t outn_clamp(int written, size_t cap) {
    size_t n = written < 0 ? 0 : (size_t)written;
    return n >= cap ? cap - 1 : n;
}

static int exec_captured(const char *line, char *out, size_t cap, size_t *outn, bool check_allow) {
    if (cap < 5) { *outn = 0; return 255; } /* too small even for "...\n" + NUL */
    char copy[136];
    snprintf(copy, sizeof(copy), "%s", line);
    char *argv[6];
    int argc = tokenize(copy, argv);
    if (argc == 0 || (check_allow && !remote_allowed(argc, argv))) {
        *outn = outn_clamp(snprintf(out, cap, "console only"), cap);
        return 255;
    }
    console_out_fn saved = g_out;
    void *saved_ctx = g_out_ctx;
    rcap = out;
    rcap_cap = cap > 5 ? cap - 5 : 0;   /* room for "...\n" and a NUL */
    rcap_len = 0;
    rcap_trunc = false;
    g_out = remote_out;
    g_out_ctx = NULL;
    g_raw = line;
    int rc = dispatch(argc, argv);
    g_out = saved;
    g_out_ctx = saved_ctx;
    out[rcap_len] = '\0';
    if (rc != 0) {
        /* cerr() printed "err <msg>\n"; hand back just <msg>. */
        const char *msg = "failed";
        for (char *p = out; p && *p; ) {
            if (strncmp(p, "err ", 4) == 0) msg = p + 4;
            p = strchr(p, '\n');
            if (p) p++;
        }
        char m[96];
        size_t ml = strcspn(msg, "\n");
        if (ml > 80) ml = 80;
        memcpy(m, msg, ml);
        m[ml] = '\0';
        int code = strncmp(m, "usage", 5) == 0 ? 10 : strcmp(m, "bad drive") == 0 ? 101 : 255;
        *outn = outn_clamp(snprintf(out, cap, "%s", m), cap);
        return code;
    }
    if (rcap_trunc) {
        /* spec §4.2: the marker is its own line, so drop the trailing
         * partial line (if any) rather than splicing "..." onto it. */
        while (rcap_len > 0 && out[rcap_len - 1] != '\n') rcap_len--;
        memcpy(out + rcap_len, "...\n", 4);
        rcap_len += 4;
    }
    *outn = rcap_len;
    return 0;
}

int console_exec_remote(void *ctx, const char *line, char *out, size_t cap, size_t *outn) {
    (void)ctx;
    return exec_captured(line, out, cap, outn, true);
}

int console_exec_capture(const char *line, char *msg, size_t cap) {
    size_t n;
    return exec_captured(line, msg, cap, &n, false) == 0 ? 0 : -1;
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
    if (n < 0) { rom_fallback(); return -1; }
    buf[n] = '\0';
    int count = 0;
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save);
    while (line) {
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\r') line[l - 1] = '\0';
        if (line[0] != '\0' && line[0] != '#') {
            if (console_exec(line) != 0) {
                if (strncasecmp(line, "net psk", 7) == 0) LOG_E(LOG_M_CONSOLE, "config: net psk <redacted>");
                else LOG_E(LOG_M_CONSOLE, "config: %s", line);
            }
            count++;
        }
        line = strtok_r(NULL, "\n", &save);
    }
    rom_fallback();
    return count;
}
