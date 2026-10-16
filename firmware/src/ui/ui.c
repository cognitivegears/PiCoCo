#include "ui.h"
#include "plat.h"
#include "rom.h"
#include "mode.h"
#include "net.h"
#include "console.h"
#include "ui_text.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef PICOCO_VERSION
#define PICOCO_VERSION "dev"
#endif

/* Screens are a draw/key pair each (the table at the bottom); a new screen
 * is one more pair. Every change a screen makes is saved at once (ADR 0011). */
#define MAX_NAMES 128
#define NAME_LEN  32            /* longest name kept, with its NUL */
#define LIST_TOP  2
#define LIST_ROWS 12
#define MSG_ROW   14
#define POLL_QUIET_MS 250       /* a poll cut short is dropped after this; the stub retries at ~1.3 s */
#define JUMP_MS   1000          /* G+letter within this narrows the jump prefix */

typedef enum { SCR_HOME, SCR_ROMS, SCR_DISKS, SCR_SETTINGS, SCR_WIFI } screen_t;

static ui_send_fn g_send;
static void *g_ctx;
static ui_exec_fn g_exec;
static dw_store *g_store;
static dw_server *g_dw;
static bool active;

static uint8_t scr[UI_CELLS];   /* what the screen should show, VDG screen codes */
static uint8_t sent[UI_CELLS];  /* what the last reply leaves on the CoCo */
static uint8_t ack[UI_CELLS];   /* what the CoCo is known to show */
static uint8_t reply[UI_REPLY_MAX + 4];
static size_t reply_len;

static uint8_t caps;
static screen_t screen;
static int menu_sel;
static char msg[UI_COLS + 1];
static char pending[NAME_LEN];
static uint8_t pending_act;
static uint32_t now_ms;

/* One list, reloaded when a list screen opens. */
static char names[MAX_NAMES][NAME_LEN];
static int nnames, sel, top;
static bool truncated, list_roms;
static char jump[5];
static int jumpn;
static bool jump_armed;
static uint32_t jump_ms;
static bool eject_armed;        /* Disks: E pressed, a digit picks the drive */

static uint8_t pk[5];
static int pkn;
static uint32_t pk_ms;

/* ASCII to VDG screen code. Normal video is $40-$7F, inverse $00-$3F.
 * ponytail: lowercase folds to uppercase; real lowercase entry is the next
 * plan's line editor. */
static uint8_t code(char c, bool inv) {
    uint8_t u = (uint8_t)c;
    if (u >= 0x60 && u < 0x80) u -= 0x20;
    if (u < 0x20 || u >= 0x80) u = ' ';
    uint8_t s = u < 0x40 ? (uint8_t)(u + 0x40) : u;
    return inv ? (uint8_t)(s & 0x3F) : s;
}

static void put(int row, int col, const char *s, bool inv) {
    for (int i = row * UI_COLS + col; *s && i < (row + 1) * UI_COLS; i++) scr[i] = code(*s++, inv);
}

static void put_right(int row, const char *s) {
    int l = (int)strlen(s);
    put(row, l < UI_COLS ? UI_COLS - l : 0, s, false);
}

void ui_row_text(int row, char out[UI_COLS + 1]) {
    for (int c = 0; c < UI_COLS; c++) {
        uint8_t s = scr[row * UI_COLS + c];
        if (s < 0x20) s += 0x40;            /* inverse letters */
        else if (s >= 0x60) s -= 0x40;      /* normal punctuation and digits */
        out[c] = (char)s;                   /* $20-$3F inverse digits, $40-$5F normal letters: as is */
    }
    out[UI_COLS] = '\0';
}

uint8_t ui_cell(int row, int col) { return scr[row * UI_COLS + col]; }
void ui_set_cell(int row, int col, uint8_t c) { scr[row * UI_COLS + col] = c; }

static void set_msg(const char *s) { snprintf(msg, sizeof msg, "%s", s); }

/* Run a console line; on failure the reason is the message row. 0 ok. */
static int run(const char *line) {
    char err[UI_COLS + 1] = "FAILED";
    if (g_exec(line, err, sizeof err) != 0) { set_msg(err); return -1; }
    return 0;
}

/* A change is run and then saved: the Manager has no unsaved state. */
static int change(const char *line) {
    if (run(line) != 0) return -1;
    return run("save");
}

/* "YYYY-MM-DD HH:MM" to unix seconds (the clock holds local time as if UTC). 0 ok. */
static int parse_datetime(const char *s, int64_t *out) {
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    static const int at[5] = { 0, 5, 8, 11, 14 }, w[5] = { 4, 2, 2, 2, 2 };
    int f[5];
    if (strlen(s) != 16 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':') return -1;
    for (int k = 0; k < 5; k++) {
        f[k] = 0;
        for (int i = 0; i < w[k]; i++) {
            if (s[at[k] + i] < '0' || s[at[k] + i] > '9') return -1;
            f[k] = f[k] * 10 + (s[at[k] + i] - '0');
        }
    }
    int y = f[0], mo = f[1], d = f[2];
#define LEAP(v) (((v) % 4 == 0 && (v) % 100 != 0) || (v) % 400 == 0)
    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || f[3] > 23 || f[4] > 59) return -1;
    if (d > mdays[mo - 1] + (mo == 2 && LEAP(y))) return -1;
    int64_t days = d - 1;
    for (int i = 1970; i < y; i++) days += LEAP(i) ? 366 : 365;
    for (int i = 1; i < mo; i++) days += mdays[i - 1] + (i == 2 && LEAP(y));
#undef LEAP
    *out = days * 86400 + f[3] * 3600 + f[4] * 60;
    return 0;
}

/* "+HH:MM", "-HH", "OFF" (or empty) to minutes east. 0 ok, 1 off, -1 bad. Within +-14:00, as net_set_tz. */
static int parse_tz(const char *s, int *minutes) {
    bool neg = false;
    int h = 0, m = 0, nd = 0;
    if (!s[0] || strcasecmp(s, "off") == 0) return 1;
    if (*s == '+' || *s == '-') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9' && nd < 2) { h = h * 10 + (*s++ - '0'); nd++; }
    if (!nd) return -1;
    if (*s == ':') {
        s++;
        if (!(s[0] >= '0' && s[0] <= '5' && s[1] >= '0' && s[1] <= '9')) return -1;
        m = (s[0] - '0') * 10 + (s[1] - '0');
        s += 2;
    }
    if (*s || h > 14 || (h == 14 && m)) return -1;
    m += h * 60;
    *minutes = neg ? -m : m;
    return 0;
}

/* ---- list widget: ROMs and Disks share it ---- */

static void list_cb(const char *name, uint32_t size, void *ctx) {
    (void)size; (void)ctx;
    size_t l = strlen(name);
    if (l >= NAME_LEN) return;
    bool rom = l > 4 && strcasecmp(name + l - 4, ".rom") == 0;
    /* `rom boot` takes one token, so a ROM name with a space cannot be sent. */
    if (list_roms ? (!rom || strchr(name, ' ') != NULL) : (rom || dw_disk_is_config_name(name))) return;
    if (nnames >= MAX_NAMES) { truncated = true; return; }
    memcpy(names[nnames++], name, l + 1);
}

static int name_cmp(const void *a, const void *b) { return strcasecmp(a, b); }

static void list_load(bool roms) {
    nnames = 0;
    truncated = false;
    list_roms = roms;
    plat_fs_list(list_cb, NULL);
    qsort(names, (size_t)nnames, NAME_LEN, name_cmp);
    sel = top = 0;
    jumpn = 0;
    jump_armed = false;
}

/* Drive holding this image, or -1. */
static int mounted_in(const char *name) {
    for (int i = 0; i < DW_MAX_DRIVES; i++)
        if (g_dw->drives[i].mounted && strcasecmp(g_dw->drives[i].name, name) == 0) return i;
    return -1;
}

static void list_draw(const char *title, const char *empty) {
    put(1, 0, nnames ? title : empty, false);
    if (nnames > LIST_ROWS) {
        char c[40];
        int last = top + LIST_ROWS < nnames ? top + LIST_ROWS : nnames;
        snprintf(c, sizeof c, "%d-%d OF %d", top + 1, last, nnames);
        put_right(1, c);
    }
    for (int i = 0; i < LIST_ROWS && top + i < nnames; i++) {
        put(LIST_TOP + i, 1, names[top + i], top + i == sel);
        int d = list_roms ? -1 : mounted_in(names[top + i]);
        if (d >= 0) put(LIST_TOP + i, 0, (char[]){ (char)('0' + d), 0 }, false);
    }
    put(MSG_ROW, 0, msg[0] ? msg : truncated ? "LIST TRUNCATED" : "", false);
}

static bool list_key(uint8_t key) {
    if (jump_armed && key) {
        jump_armed = false;
        if (now_ms - jump_ms > JUMP_MS) jumpn = 0;
        if (jumpn < 4) jump[jumpn++] = (char)(key >= 'a' && key <= 'z' ? key - 0x20 : key);
        jump[jumpn] = '\0';
        jump_ms = now_ms;
        for (int i = 0; i < nnames; i++)
            if (strncasecmp(names[i], jump, (size_t)jumpn) == 0) { sel = top = i; break; }   /* the target heads the window */
    }
    else if (key == UI_KEY_UP) { if (sel > 0) sel--; }
    else if (key == UI_KEY_DOWN) { if (sel + 1 < nnames) sel++; }
    else if (key == UI_KEY_PGUP) sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;
    else if (key == UI_KEY_PGDN) sel = sel + LIST_ROWS < nnames ? sel + LIST_ROWS : (nnames ? nnames - 1 : 0);
    else if (key == 'G' || key == 'g') jump_armed = true;
    else return false;
    if (sel < top) top = sel;
    if (sel >= top + LIST_ROWS) top = sel - LIST_ROWS + 1;
    return true;
}

/* ---- leaving ---- */

static uint8_t leave_warm(void) {
    pending[0] = '\0';
    pending_act = (caps & UI_CAP_COCO3) ? UI_ACT_WARM3 : UI_ACT_WARM;
    return pending_act;
}

/* ---- home ---- */

static void draw_header(void) {
    char line[UI_COLS + 1];
    if (caps & UI_CAP_COCO3)    /* every CoCo 3 has ECB; the size is in bits 6-7 */
        snprintf(line, sizeof line, "PICOCO  COCO 3 %s",
                 (const char *[]){ "128K", "512K", "1M", "2M" }[(caps & UI_CAP_MEM3_MASK) >> 6]);
    else
        snprintf(line, sizeof line, "PICOCO  %s %s",
                 caps & UI_CAP_64K ? "64K" : caps & UI_CAP_32K ? "32K" : "16K",
                 caps & UI_CAP_ECB ? "ECB" : "NO ECB");
    put(0, 0, line, false);
    snprintf(line, sizeof line, "%s " PICOCO_VERSION, mode_name(mode_get()));
    for (char *p = line; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 0x20;
    put_right(0, line);
}

static void draw_home(void) {
    static const char *const items[] = { "ROMS", "DISKS", "SETTINGS", "WIFI" };
    char line[UI_COLS + 1];
    for (int i = 0; i < DW_MAX_DRIVES; i++) {
        dw_disk *d = &g_dw->drives[i];
        snprintf(line, sizeof line, "D%d %s", i, d->mounted ? d->name : "-");
        put(2 + i, 0, line, false);
        if (d->mounted && d->read_only) put_right(2 + i, "RO");
    }
    const char *next = console_rom_next();
    if (strncmp(next, "load ", 5) == 0) next += 5;
    snprintf(line, sizeof line, "ROM NEXT %s", next[0] ? next : "MANAGER");
    put(6, 0, line, false);
    for (int i = 0; i < 4; i++) put(8 + i, 1, items[i], i == menu_sel);
    put(11, 8, !net_available() ? "NO RADIO" : net_state() == NET_UP ? net_ip() : net_state_name(net_state()), false);
    put(MSG_ROW, 0, msg, false);
    put(15, 0, "ENTER=OPEN  BREAK=BASIC", false);
}

static uint8_t key_home(uint8_t key) {
    if (key == UI_KEY_UP && menu_sel > 0) menu_sel--;
    else if (key == UI_KEY_DOWN && menu_sel < 3) menu_sel++;
    else if (key == UI_KEY_BREAK) return leave_warm();
    else if (key == UI_KEY_ENTER) {
        screen = (screen_t)(SCR_ROMS + menu_sel);
        if (screen == SCR_ROMS || screen == SCR_DISKS) list_load(screen == SCR_ROMS);
        eject_armed = false;
    }
    return 0;
}

/* ---- ROMs ---- */

/* First two bytes of a file: "DK" marks a DOS ROM that Extended BASIC must start. */
static int peek2(const char *name, uint8_t two[2]) {
    dw_file f;
    if (g_store->ops->open(g_store->ctx, name, false, &f) < 0) return -1;
    int n = g_store->ops->read(&f, 0, two, 2);
    g_store->ops->close(&f);
    return n == 2 ? 0 : -1;
}

static uint8_t launch(void) {
    if (!nnames) return 0;
    const char *name = names[sel];
    int rc = rom_check_file(g_store, name);
    uint8_t two[2];
    if (rc == -2) { set_msg("NOT A ROM SIZE"); return 0; }
    if (rc != 0 || peek2(name, two) != 0) { set_msg("CANNOT READ FILE"); return 0; }
    bool dos = two[0] == 'D' && two[1] == 'K';
    if (dos && !(caps & UI_CAP_ECB)) { set_msg("NEEDS EXTENDED BASIC"); return 0; }
    snprintf(pending, sizeof pending, "%s", name);
    /* A CoCo 3 copies the cart to RAM at boot and runs all-RAM: a pak is
     * started in ROM mode like the CoCo 3's own cart start ($8C28), a DOS ROM
     * by re-running reset so the new cart is copied. */
    bool c3 = (caps & UI_CAP_COCO3) != 0;
    pending_act = dos ? (c3 ? UI_ACT_COLD3 : UI_ACT_COLD) : (c3 ? UI_ACT_JUMP3 : UI_ACT_JUMP);
    return pending_act;
}

static void draw_roms(void) {
    list_draw("ROMS", "NO .ROM FILES");
    put(15, 0, "ENTER=RUN  D=DEFAULT  BREAK=BACK", false);
}

static uint8_t key_roms(uint8_t key) {
    if (list_key(key)) return 0;
    if (key == UI_KEY_ENTER) return launch();
    if (key == UI_KEY_BREAK) screen = SCR_HOME;
    else if ((key == 'D' || key == 'd') && nnames) {
        char line[16 + NAME_LEN];
        snprintf(line, sizeof line, "rom boot %s", names[sel]);
        if (change(line) == 0) set_msg("NEXT BOOT SET");
    }
    return 0;
}

/* ---- Disks ---- */

static void draw_disks(void) {
    list_draw("DISKS", "NO DISK IMAGES");
    put(15, 0, "0-3=MOUNT  E=EJECT  N=NEW  B=BOOT", false);
}

static void new_disk_cb(const char *text) {
    static char line[48];
    if (!text[0]) return;
    snprintf(line, sizeof line, "fs new %s%s", text, strchr(text, '.') ? "" : ".DSK");
    if (strlen(line) - 7 >= NAME_LEN) { set_msg("NAME TOO LONG"); return; }   /* the list keeps names under NAME_LEN */
    if (change(line) == 0) list_load(false);
}

static uint8_t key_disks(uint8_t key) {
    char line[24 + NAME_LEN];
    if (list_key(key)) return 0;
    if (key == UI_KEY_BREAK) screen = SCR_HOME;
    else if (key >= '0' && key < '0' + DW_MAX_DRIVES) {
        if (eject_armed) snprintf(line, sizeof line, "dw disk eject %c", key);
        else if (nnames) snprintf(line, sizeof line, "dw disk insert %c %s", key, names[sel]);
        else return 0;
        eject_armed = false;
        change(line);
    }
    else if (key == 'E' || key == 'e') { eject_armed = true; set_msg("EJECT: PRESS 0-3"); }
    else if (key == 'N' || key == 'n') ui_text_begin("NAME:", "", false, 31, new_disk_cb);
    else if (key == 'B' || key == 'b') set_msg("NOT YET");   /* #66 Boot */
    return 0;
}

/* ---- Settings ---- */

static void clock_text(char when[20]) {
    time_t t = (time_t)dw_time_get(g_dw, now_ms);
    struct tm tmv;
    if (gmtime_r(&t, &tmv)) strftime(when, 20, "%Y-%m-%d %H:%M", &tmv);   /* dw time is local already */
    else snprintf(when, 20, "?");
}

static void clock_cb(const char *text) {
    static char line[32];
    int64_t t;
    if (parse_datetime(text, &t) != 0) { set_msg("BAD DATE/TIME"); return; }
    snprintf(line, sizeof line, "time set %lld", (long long)t);
    change(line);
}

static void tz_cb(const char *text) {
    static char line[24];
    int m = 0;
    int r = parse_tz(text, &m);
    if (r < 0) { set_msg("BAD OFFSET"); return; }
    if (r) snprintf(line, sizeof line, "net tz off");
    else snprintf(line, sizeof line, "net tz %d", m);
    change(line);
}

static void draw_settings(void) {
    char line[UI_COLS + 1], when[20];
    clock_text(when);
    put(1, 0, "SETTINGS", false);
    snprintf(line, sizeof line, "H:HDB-DOS DRIVE MODE %s", g_dw->hdbdos ? "ON" : "OFF");
    put(3, 0, line, false);
    snprintf(line, sizeof line, "T:CLOCK %s", when);
    put(4, 0, line, false);
    int tz = net_tz();
    if (tz == NET_TZ_OFF) snprintf(line, sizeof line, "Z:TIMEZONE OFF");
    else snprintf(line, sizeof line, "Z:TIMEZONE UTC%c%02d:%02d", tz < 0 ? '-' : '+', abs(tz) / 60, abs(tz) % 60);
    put(5, 0, line, false);
    int64_t dummy;
    if (!plat_rtc_get(&dummy)) put(6, 2, "(CLOCK LOST AT RESET)", false);
    put(MSG_ROW, 0, msg, false);
    put(15, 0, "BREAK=BACK", false);
}

static uint8_t key_settings(uint8_t key) {
    if (key == UI_KEY_BREAK) screen = SCR_HOME;
    else if (key == 'H' || key == 'h') change(g_dw->hdbdos ? "dw hdbdos off" : "dw hdbdos on");
    else if (key == 'T' || key == 't') {
        char when[20];
        clock_text(when);
        ui_text_begin("YYYY-MM-DD HH:MM", when, false, 16, clock_cb);
    }
    else if (key == 'Z' || key == 'z') ui_text_begin("UTC OFFSET (+HH:MM, -HH, OFF):", "", false, 6, tz_cb);
    return 0;
}

/* ---- WiFi ---- */

static bool scanning;           /* WiFi: the list holds scanned SSIDs, ENTER picks one */
static char ssid_pick[NAME_LEN], host_pick[UI_TEXT_MAX + 1];
static char wline[96];

static void psk_cb(const char *text) {
    snprintf(wline, sizeof wline, "net psk%s%s", text[0] ? " " : "", text);
    change(wline);
}

static void join_cb(const char *text) {
    snprintf(wline, sizeof wline, "net join %s", ssid_pick);
    if (run(wline) == 0) psk_cb(text);      /* one action, one save: psk_cb's */
}

static void port_cb(const char *text) {
    unsigned port = text[0] ? 0 : NET_DEFAULT_PORT;
    for (const char *c = text; *c; c++) {
        if (*c < '0' || *c > '9') { port = 0; break; }
        port = port * 10 + (unsigned)(*c - '0');
        if (port > 65535) { port = 0; break; }
    }
    if (!port) { set_msg("BAD PORT"); return; }
    snprintf(wline, sizeof wline, "net server %s %u", host_pick, port);
    change(wline);
}

static void host_cb(const char *text) {
    if (!text[0]) return;
    snprintf(host_pick, sizeof host_pick, "%s", text);
    ui_text_begin("PORT (65504):", "", false, 5, port_cb);
}

static void scan_cb(const char *ssid, int rssi, int chan, void *ctx) {
    (void)rssi; (void)chan; (void)ctx;
    size_t l = strlen(ssid);
    /* shortcut: a 32-character SSID does not fit a list name and is left out; widen NAME_LEN if one turns up. */
    if (!l || l >= NAME_LEN || nnames >= MAX_NAMES) return;
    for (int i = 0; i < nnames; i++) if (strcmp(names[i], ssid) == 0) return;   /* one row per network, not per access point */
    memcpy(names[nnames++], ssid, l + 1);
}

static void scan_start(void) {
    if (mode_get() == MODE_NET) { set_msg("LEAVE NET MODE FIRST"); return; }
    nnames = sel = top = jumpn = 0;
    truncated = jump_armed = false;
    list_roms = true;                   /* no drive digits beside SSIDs */
    /* shortcut: net_scan blocks core0 for up to 5 s; the stub's poll retries cover it. */
    if (net_scan(scan_cb, NULL) != 0) { set_msg("SCAN FAILED"); return; }
    if (!nnames) { set_msg("NO NETWORKS FOUND"); return; }
    scanning = true;
}

static void draw_wifi(void) {
    char line[UI_COLS + 1];
    if (scanning) {
        list_draw("PICK A NETWORK", "");
        put(15, 0, "ENTER=PICK  BREAK=BACK", false);
        return;
    }
    put(1, 0, "WIFI", false);
    if (!net_available()) { put(3, 0, "NO RADIO ON THIS BOARD", false); put(15, 0, "BREAK=BACK", false); return; }
    snprintf(line, sizeof line, "S:SSID %s", net_ssid());
    put(3, 0, line, false);
    snprintf(line, sizeof line, "P:PSK %s", net_psk_set() ? "********" : "");
    put(4, 0, line, false);
    snprintf(line, sizeof line, "H:SERVER %s %u", net_host(), (unsigned)net_port());
    put(5, 0, line, false);
    picoco_mode boot = console_boot_mode();
    snprintf(line, sizeof line, "M:MODE %s", boot == MODE_NET ? "NET" : "NATIVE");
    put(6, 0, line, false);
    if (boot != mode_get()) put(7, 2, "(RESET TO APPLY)", false);
    snprintf(line, sizeof line, "STATE %s", net_state_name(net_state()));
    put(9, 0, line, false);
    snprintf(line, sizeof line, "IP %s", net_ip());
    put(10, 0, line, false);
    put(11, 0, net_last_error(), false);
    put(MSG_ROW, 0, msg, false);
    put(15, 0, "BREAK=BACK", false);
}

static uint8_t key_wifi(uint8_t key) {
    if (scanning) {
        if (list_key(key)) return 0;
        if (key == UI_KEY_BREAK) scanning = false;
        else if (key == UI_KEY_ENTER) {
            snprintf(ssid_pick, sizeof ssid_pick, "%s", names[sel]);
            scanning = false;
            ui_text_begin("PSK:", "", true, NET_PSK_MAX, join_cb);
        }
        return 0;
    }
    if (key == UI_KEY_BREAK) screen = SCR_HOME;
    else if (!net_available()) return 0;
    else if (key == 'M' || key == 'm') change(console_boot_mode() == MODE_NET ? "net mode native" : "net mode net");
    else if (key == 'S' || key == 's') scan_start();
    else if (key == 'P' || key == 'p') ui_text_begin("PSK:", "", true, NET_PSK_MAX, psk_cb);
    else if (key == 'H' || key == 'h') ui_text_begin("HOST:", "", false, 63, host_cb);
    return 0;
}

static const struct { void (*draw)(void); uint8_t (*key)(uint8_t); } screens[] = {
    [SCR_HOME] = { draw_home, key_home },
    [SCR_ROMS] = { draw_roms, key_roms },
    [SCR_DISKS] = { draw_disks, key_disks },
    [SCR_SETTINGS] = { draw_settings, key_settings },
    [SCR_WIFI] = { draw_wifi, key_wifi },
};

static void draw(void) {
    memset(scr, 0x60, sizeof scr);
    draw_header();
    screens[screen].draw();
    ui_text_draw();                 /* the editor overlays rows 12-15 */
}

/* Emit the cells that differ from ack as text runs, then act (if any) and
 * the end marker. Runs join across gaps under 4 cells (a run header is 4
 * bytes). Worst case is every cell changed: 3 runs, 524 bytes, inside
 * UI_REPLY_MAX; the bound check is a guard, and a cell it leaves out stays
 * different in `sent` so the next poll sends it. */
static void build_reply(bool full, uint8_t act) {
    uint8_t *p = reply + 2, *end = reply + 2 + UI_REPLY_MAX - 2;
    if (full) { *p++ = UI_ACT_CLEAR; memset(ack, 0x60, sizeof ack); }
    memcpy(sent, ack, sizeof sent);
    for (int i = 0; i < UI_CELLS; ) {
        if (scr[i] == ack[i]) { i++; continue; }
        int last = i;
        for (int j = i + 1; j < UI_CELLS && j - last < 4 && j - i < 255; j++)
            if (scr[j] != ack[j]) last = j;
        int n = last - i + 1;
        if (p + 4 + n > end) break;
        *p++ = UI_ACT_TEXT; *p++ = (uint8_t)(i >> 8); *p++ = (uint8_t)i; *p++ = (uint8_t)n;
        memcpy(p, scr + i, (size_t)n);
        memcpy(sent + i, scr + i, (size_t)n);
        p += n;
        i += n;
    }
    if (act) *p++ = act;
    *p++ = UI_ACT_END;
    size_t len = (size_t)(p - (reply + 2));
    unsigned sum = 0;
    for (size_t k = 0; k < len; k++) sum += reply[2 + k];
    reply[0] = (uint8_t)(len >> 8); reply[1] = (uint8_t)len;
    reply[2 + len] = (uint8_t)(sum >> 8); reply[3 + len] = (uint8_t)sum;
    reply_len = len + 4;
    g_send(g_ctx, reply, reply_len);
}

static uint8_t on_key(uint8_t key) {
    if (key) { msg[0] = '\0'; pending_act = 0; }
    if (ui_text_active()) { ui_text_key(key); return 0; }
    return screens[screen].key(key);
}

static void on_poll(uint8_t flags, uint8_t key, uint8_t c) {
    if ((flags & 1) && reply_len) { g_send(g_ctx, reply, reply_len); return; }
    bool first = (flags & 2) != 0;
    uint8_t act = 0;
    if (first) { caps = c; screen = SCR_HOME; menu_sel = 0; scanning = false; ui_text_key(UI_KEY_BREAK); }
    else { memcpy(ack, sent, sizeof ack); act = on_key(key); }   /* a new poll acknowledges the last reply */
    if (act) memset(scr, 0x60, sizeof scr);   /* leaving: the reply clears the screen, nothing to draw */
    else draw();
    build_reply(first || act, act);
}

static void on_go(void);

void ui_feed(const uint8_t *buf, size_t n, uint32_t now) {
    now_ms = now;
    if (pkn && now_ms - pk_ms > POLL_QUIET_MS) pkn = 0;
    pk_ms = now_ms;
    for (size_t i = 0; i < n; i++) {
        uint8_t b = buf[i];
        if (pkn == 0) {
            if (b == 'G') { on_go(); continue; }
            if (b != 'P') continue;                 /* noise: wait for a poll */
        }
        pk[pkn++] = b;
        int want = (pkn >= 2 && (pk[1] & 2)) ? 5 : 4;   /* P flags key [caps] sum8 */
        if (pkn < 2 || pkn < want) continue;
        uint8_t sum = 0;
        for (int k = 0; k < want - 1; k++) sum += pk[k];
        pkn = 0;
        if (sum != pk[want - 1]) continue;          /* no reply: the stub times out and asks again */
        on_poll(pk[1], pk[2], want == 5 ? pk[3] : caps);
    }
}

void ui_ctl(uint8_t codev) {
    if (codev == 0xA5) { active = true; pkn = 0; reply_len = 0; pending_act = 0; }
    else if (codev == 0x5A) active = false;
}

bool ui_active(void) { return active; }

void ui_init(ui_send_fn send, void *ctx, ui_exec_fn exec, dw_store *store, dw_server *dw) {
    g_send = send; g_ctx = ctx; g_exec = exec; g_store = store; g_dw = dw;
    active = false;
    pkn = 0; reply_len = 0; pending_act = 0;
    screen = SCR_HOME; menu_sel = 0; scanning = false; ui_text_key(UI_KEY_BREAK);
    nnames = sel = top = 0; truncated = false;
    msg[0] = '\0'; pending[0] = '\0'; caps = 0;
    memset(scr, 0x60, sizeof scr);
    memset(sent, 0x60, sizeof sent);
    memset(ack, 0x60, sizeof ack);
}

/* The stub has left the cart and is running from RAM: do the swap now. */
static void on_go(void) {
    uint8_t r = UI_GO_OK;
    if (!pending_act) r = UI_GO_FAIL;
    else if (pending[0]) {
        char line[16 + NAME_LEN], err[UI_COLS + 1] = "ROM LOAD FAILED";
        snprintf(line, sizeof line, "rom launch %s", pending);
        if (g_exec(line, err, sizeof err) != 0) { r = UI_GO_FAIL; set_msg(err); }
    }
    pending_act = 0;
    if (r == UI_GO_OK) { msg[0] = '\0'; active = false; }
    g_send(g_ctx, &r, 1);
}
