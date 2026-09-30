#include <coco.h>
#include "ui.h"
#include "dw.h"
#include "boot.h"

#define VRAM ((u8 *)0x0400)
#define COLS 32
#define LIST_TOP 4
#define LIST_ROWS 9
#define MSG_ROW 14
#define TICKS (*(volatile u16 *)0x0112)
#define ARYEND (*(volatile u16 *)0x001F)   /* BASIC end-of-arrays pointer */

/* "fs ls" body (capped at 3072 + 23-byte OK header + NUL here; the firmware
 * may send up to 4096, see ls_cut) lives at a fixed address instead of
 * BSS, between the 2-line PICOCO.BAS loader ($2601) and the program image
 * ($3800): frees 3096 bytes of program RAM. Safe because
 * boot.c's OS-9 boot (track 34 -> $2600) and RUN"X"/LOADM"X" handoff only
 * touch this region after the picked file's name has already been copied
 * out of it, i.e. once the list is no longer needed. ui_run() refuses to
 * start if BASIC's own arrays already reach into this region. */
#define LSBUF ((char *)0x2700)
#define LSBUF_SIZE 3096
/* The freed tail of the $2700-$37FF window (same lifetime as LSBUF) holds
 * FILES[] (128 * 4 = 512 bytes) and the reply buffer. */
#define FILES ((file_ent *)0x3318)
#define REPLY ((char *)0x3518)
#define REPLY_SIZE 736
#if MAX_FILES * 4 > 0x200
#error FILES[] no longer fits before REPLY
#endif

u8 ui_dirty;
static int nfiles, sel, top;
static char drives[4][32];
static char fw[12];
static char line[160];

/* VDG text: bit 6 set = normal (dark on green), clear = inverse; lowercase
 * shows as inverse uppercase on a CoCo 1/2, so fold to uppercase. */
static u8 vdg(char c, u8 inv)
{
    u8 u = (u8)c;
    if (u >= 'a' && u <= 'z') u = (u8)(u - 32);
    if (u < 0x20 || u > 0x5F) u = '.';
    u &= 0x3F;
    return inv ? u : (u8)(u | 0x40);
}

void clear_row(u8 row) { memset(VRAM + (u16)row * COLS, 0x60, COLS); }
void clear_screen(void) { memset(VRAM, 0x60, 512); }

void put_at(u8 row, u8 col, const char *s, u8 inv)
{
    u8 *p = VRAM + (u16)row * COLS + col;
    while (*s && col < COLS) { *p++ = vdg(*s++, inv); col++; }
}

u8 key(void)
{
    u8 k;
    while ((k = inkey()) == 0) ;
    return k;
}

/* Letters arrive as $61-$7A in the CoCo's lowercase mode (SHIFT+0); fold
 * them so every key binding works in either mode. */
static u8 upcase(u8 k)
{
    return (k >= 'a' && k <= 'z') ? (u8)(k - 32) : k;
}

void msg(const char *s)
{
    clear_row(MSG_ROW);
    put_at(MSG_ROW, 0, s, 1);
    key();
}

static void msg2(const char *l1, const char *l2)
{
    clear_row(MSG_ROW); clear_row(MSG_ROW + 1);
    put_at(MSG_ROW, 0, l1, 1);
    put_at(MSG_ROW + 1, 0, l2, 1);
    key();
}

int ui_cmd(const char *l, char **body)
{
    int rc = picoco_cmd(l, REPLY, REPLY_SIZE, body);
    if (rc == PC_TIMEOUT) msg2("PICOCO NOT RESPONDING", "(BECKER NATIVE? FIRMWARE >= 1.2?)");
    else if (rc == PC_BAD) msg("BAD REPLY (FIRMWARE >= 1.2?)");
    else if (rc == PC_TOOLONG) msg("NAME TOO LONG");
    else if (rc > 0) msg(*body);
    return rc;
}

int input_line(const char *prompt, char *buf, int max)
{
    int n = 0, i;
    u8 k, col = (u8)strlen(prompt);
    clear_row(MSG_ROW); clear_row(MSG_ROW + 1);
    put_at(MSG_ROW, 0, prompt, 0);
    for (;;) {                      /* text runs on across rows MSG_ROW and MSG_ROW+1 */
        buf[n] = '\0';
        memset(VRAM + MSG_ROW * COLS + col, 0x60, 2 * COLS - col);
        for (i = 0; i < n; i++) VRAM[MSG_ROW * COLS + col + i] = vdg((u8)buf[i], 0);
        VRAM[MSG_ROW * COLS + col + n] = 0x20;   /* inverse-space cursor */
        k = key();
        if (k == 13) return n;
        if (k == 3) return -1;
        if (k == 8) { if (n) n--; continue; }
        if (k >= 32 && k < 127 && n < max && col + n < 2 * COLS - 1) buf[n++] = (char)k;
    }
}

static void draw_row(u8 row, file_ent *e, u8 inv)
{
    char kb[8];
    clear_row(row);
    if (inv) memset(VRAM + (u16)row * COLS, 0x20, COLS);
    put_at(row, 1, e->name, inv);
    if (e->kb) {
        u32_to_dec(e->kb, kb);
        strcat(kb, "K");
        put_at(row, (u8)(COLS - strlen(kb)), kb, inv);
        VRAM[(u16)row * COLS + (COLS - strlen(kb) - 1)] = inv ? 0x20 : 0x60;   /* gap before the size */
    }
}

static void draw_list(file_ent *e, int n, int s, int t, u8 row0, u8 rows)
{
    u8 r;
    for (r = 0; r < rows; r++) {
        if (t + r < n) draw_row((u8)(row0 + r), &e[t + r], (u8)(t + r == s));
        else clear_row((u8)(row0 + r));
    }
}

static int clamp_top(int s, int t, int rows)
{
    if (s < t) return s;
    if (s >= t + rows) return s - rows + 1;
    return t;
}

int pick_list(const char *title, file_ent *e, int n)
{
    int s = 0, t = 0;
    u8 k;
    clear_screen();
    put_at(0, 0, title, 0);
    put_at(15, 0, "ENTER:PICK  BREAK:CANCEL", 0);
    for (;;) {
        t = clamp_top(s, t, 12);
        draw_list(e, n, s, t, 2, 12);
        k = key();
        if (k == 3) return -1;
        if (k == 13 && n) return s;
        if (k == 94 && s > 0) s--;
        if (k == 10 && s < n - 1) s++;
    }
}

static void load_drives(void)
{
    char *body;
    if (ui_cmd("dw disk show", &body) == 0) parse_disks(body, drives);
}

/* picoco_cmd stops at the buffer end, losing the firmware's "..." marker and
 * maybe cutting the last line: drop the partial line, report truncation. */
static int ls_cut(char *body)
{
    char *p;
    if (strlen(LSBUF) < LSBUF_SIZE - 1) return 0;
    p = LSBUF + strlen(LSBUF);
    while (p > body && p[-1] != '\n') p--;
    *p = '\0';
    return 1;
}

static void load_files(void)
{
    char *body;
    int trunc;
    nfiles = 0;
    if (picoco_cmd("fs ls", LSBUF, LSBUF_SIZE, &body) != 0) { msg("CANNOT LIST FILES"); return; }
    trunc = ls_cut(body);
    if (list_truncated(body)) trunc = 1;           /* before parse_ls rewrites body in place */
    nfiles = parse_ls(body, FILES, MAX_FILES, 0);
    sort_files(FILES, nfiles);
    if (sel >= nfiles) sel = nfiles ? nfiles - 1 : 0;
    if (trunc || nfiles == MAX_FILES) msg("LIST TRUNCATED");
}

static void draw_header(void)
{
    char d[20];
    u8 i;
    clear_row(0);
    put_at(0, 0, "PICOCO MANAGER", 0);
    put_at(0, (u8)(COLS - 3 - strlen(fw)), "FW ", 0);
    put_at(0, (u8)(COLS - strlen(fw)), fw, 0);
    clear_row(1); clear_row(2);
    for (i = 0; i < 4; i++) {
        d[0] = (char)('0' + i); d[1] = ' ';
        strncpy(d + 2, drives[i][0] ? drives[i] : "-", 13);
        d[15] = '\0';
        put_at((u8)(1 + (i & 1)), (u8)((i >> 1) * 16), d, 0);
    }
    memset(VRAM + 3 * COLS, 0x6D, COLS);            /* '-' row */
    memset(VRAM + 13 * COLS, 0x6D, COLS);
}

static void draw_help(void)
{
    clear_row(14); clear_row(15);
    put_at(14, 0, "0-3:MOUNT E:EJECT B:BOOT G:GOTO", 0);
    put_at(15, 0, "N:NEW S:SET V:SAVE BREAK:EXIT", 0);
}

static void draw_all(void)
{
    clear_screen();
    draw_header();
    top = clamp_top(sel, top, LIST_ROWS);
    if (nfiles) draw_list(FILES, nfiles, sel, top, LIST_TOP, LIST_ROWS);
    else put_at(LIST_TOP, 1, "NO DISK IMAGES ON FLASH", 0);
    draw_help();
}

static void do_mount(u8 d)
{
    char *body;
    if (!nfiles) return;
    if (strlen(FILES[sel].name) >= 32) { msg("NAME TOO LONG"); return; }
    strcpy(line, "dw disk insert ");
    line[15] = (char)('0' + d); line[16] = ' '; line[17] = '\0';
    strcat(line, FILES[sel].name);
    if (ui_cmd(line, &body) == 0) { ui_dirty = 1; load_drives(); }
}

static void do_eject(void)
{
    char *body;
    u8 k;
    clear_row(MSG_ROW);
    put_at(MSG_ROW, 0, "EJECT WHICH DRIVE (0-3)?", 1);
    k = key();
    if (k < '0' || k > '3') return;
    strcpy(line, "dw disk eject ");
    line[14] = (char)k; line[15] = '\0';
    if (ui_cmd(line, &body) == 0) { ui_dirty = 1; load_drives(); }
}

static void do_new(void)
{
    char name[28], *body;
    int i;
    if (input_line("NEW NAME: ", name, 20) <= 0) return;
    for (i = 0; name[i] && name[i] != '.'; i++) ;
    if (!name[i]) strcat(name, ".DSK");
    strcpy(line, "fs new ");
    strcat(line, name);
    if (ui_cmd(line, &body) != 0) return;
    load_files();
    for (i = 0; i < nfiles; i++) if (ends_with_ci(FILES[i].name, name) && strlen(FILES[i].name) == strlen(name)) sel = i;
}

static void do_save(void)
{
    char *body;
    if (ui_cmd("save", &body) == 0) { ui_dirty = 0; msg("SAVED"); }
}

/* Type-ahead: letters typed within a second of each other build a prefix. */
static char jump[5];
static u16 jump_t;

static void do_jump(char c)
{
    int i, l;
    if ((u16)(TICKS - jump_t) > 60) jump[0] = '\0';
    jump_t = TICKS;
    l = (int)strlen(jump);
    if (l < 4) { jump[l] = c; jump[l + 1] = '\0'; }
    l = (int)strlen(jump);
    for (i = 0; i < nfiles; i++) {
        if (strlen(FILES[i].name) >= (u16)l) {
            char save = FILES[i].name[l];
            int hit;
            FILES[i].name[l] = '\0';
            hit = ends_with_ci(FILES[i].name, jump);   /* whole prefix, case-insensitive */
            FILES[i].name[l] = save;
            if (hit) { sel = i; return; }
        }
    }
}

static int confirm_exit(void)
{
    u8 k;
    if (!ui_dirty) return 1;
    clear_row(MSG_ROW); clear_row(MSG_ROW + 1);
    put_at(MSG_ROW, 0, "NOT SAVED: V=SAVE+EXIT", 1);
    put_at(MSG_ROW + 1, 0, "BREAK=EXIT  OTHER KEY=STAY", 1);
    k = key();
    if (k == 'V' || k == 'v') { do_save(); return ui_dirty == 0; }
    return k == 3;
}

void ui_run(void)
{
    char *body;
    u8 k;
    clear_screen();
    if (ARYEND > 0x2700) { msg("BASIC PROGRAM TOO BIG"); return; }
    put_at(0, 0, "PICOCO MANAGER", 0);
    for (;;) {
        clear_row(2);
        put_at(2, 0, "CONNECTING...", 0);
        if (ui_cmd("version", &body) == 0) break;
        clear_row(MSG_ROW); clear_row(MSG_ROW + 1);
        put_at(MSG_ROW, 0, "R=RETRY  BREAK=EXIT", 1);
        for (;;) {
            k = key();
            if (k == 3) return;
            if (k == 'r' || k == 'R') break;
        }
    }
    line_value(body, "version ", fw, sizeof fw);
    load_drives();
    load_files();
    for (;;) {
        draw_all();
        k = upcase(key());
        if (k == 3) { if (confirm_exit()) { clear_screen(); return; } continue; }
        if (k == 94 && sel > 0) sel--;
        else if (k == 10 && sel < nfiles - 1) sel++;
        else if (k == 95) sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;           /* SHIFT+UP */
        else if (k == 91) sel = sel + LIST_ROWS < nfiles ? sel + LIST_ROWS : (nfiles ? nfiles - 1 : 0); /* SHIFT+DOWN */
        else if (k >= '0' && k <= '3') do_mount((u8)(k - '0'));
        else if (k == 'E') do_eject();
        else if (k == 'N') do_new();
        else if (k == 'V') do_save();
        else if (k == 'S') { settings_run(); load_drives(); load_files(); }
        else if (k == 'B' && nfiles) { if (boot_image(FILES[sel].name)) return; load_drives(); }
        else if (k == 'G') {              /* G then a letter: jump to that name */
            clear_row(MSG_ROW);
            put_at(MSG_ROW, 0, "GOTO: TYPE A LETTER", 1);
            k = upcase(key());
            if (k >= 'A' && k <= 'Z') do_jump((char)k);
        }
    }
}

static void strip_load(char *s)
{
    if (strncmp(s, "load ", 5) == 0) memmove(s, s + 5, strlen(s + 5) + 1);
}

static void fmt_time(u32 t, char *out)
{
    int y, mo, d, h, mi;
    unix_to_civil(t, &y, &mo, &d, &h, &mi);
    out[0] = (char)('0' + y / 1000); out[1] = (char)('0' + y / 100 % 10);
    out[2] = (char)('0' + y / 10 % 10); out[3] = (char)('0' + y % 10);
    out[4] = '-'; out[5] = (char)('0' + mo / 10); out[6] = (char)('0' + mo % 10);
    out[7] = '-'; out[8] = (char)('0' + d / 10); out[9] = (char)('0' + d % 10);
    out[10] = ' '; out[11] = (char)('0' + h / 10); out[12] = (char)('0' + h % 10);
    out[13] = ':'; out[14] = (char)('0' + mi / 10); out[15] = (char)('0' + mi % 10);
    out[16] = '\0';
}

static void pick_rom(void)
{
    char *body;
    int n, i;
    if (picoco_cmd("fs ls", LSBUF, LSBUF_SIZE, &body) != 0) { msg("CANNOT LIST FILES"); return; }
    ls_cut(body);
    n = parse_ls(body, FILES, MAX_FILES, 1);
    if (!n) { msg("NO .ROM FILES ON FLASH"); return; }
    sort_files(FILES, n);
    i = pick_list("ROM FOR NEXT BOOT", FILES, n);
    if (i < 0) return;
    strcpy(line, "rom boot ");
    strcat(line, FILES[i].name);
    if (ui_cmd(line, &body) == 0) { ui_dirty = 1; msg("SAVE, THEN RESET"); }
}

static void nrow(u8 r, const char *lab, const char *val)
{
    put_at(r, 0, lab, 0);
    put_at(r, 9, val[0] ? val : "(NONE)", 0);
}

static const char *const nkey[] = { "net ssid ", "net psk ", "net server ", "net state ", "net ip ", "net error " };

static void net_screen(void)
{
    char v[7][24], *body;   /* ssid psk server state ip error mode; 23 chars is all the screen shows */
    u8 k;
    int n, i, rl, pend;
    char *bm;
    for (;;) {
        if (ui_cmd("net status", &body) != 0) return;
        v[6][0] = '\0';
        line_value(body, "net radio ", v[6], 24);
        if (strcmp(v[6], "yes") != 0) { msg("NO RADIO ON THIS BOARD"); return; }
        for (i = 0; i < 6; i++) { v[i][0] = '\0'; line_value(body, nkey[i], v[i], 24); }
        line_value(body, "net mode ", v[6], 24);        /* "<running> boot <next>" */
        bm = strrchr(v[6], ' ') + 1;
        rl = (int)strlen(bm);
        pend = strncmp(v[6], bm, rl) != 0 || v[6][rl] != ' ';
        clear_screen();
        put_at(0, 0, "WIFI", 0);
        nrow(2, "S:SSID", v[0]);
        nrow(3, "P:PSK", strcmp(v[1], "set") == 0 ? "********" : "");
        nrow(4, "H:SERVER", v[2]);
        nrow(5, "M:MODE", strcmp(bm, "net") == 0 ? "NET" : "NATIVE");
        if (pend) put_at(6, 9, "(SAVE, THEN RESET)", 0);
        nrow(7, "STATE", v[3]);
        nrow(8, "IP", v[4]);
        if (v[5][0]) put_at(9, 0, v[5], 0);
        put_at(11, 0, "SHIFT+0 TOGGLES LOWERCASE", 0);
        put_at(12, 0, "V:SAVE   BREAK:BACK", 0);
        k = upcase(key());
        if (k == 3) return;
        if (k == 'S') {
            if (picoco_cmd("net scan", LSBUF, LSBUF_SIZE, &body) != 0) { msg("SCAN FAILED"); continue; }
            n = parse_scan(body, FILES, MAX_FILES);
            if (n == 0) { msg("NO NETWORKS FOUND"); continue; }
            i = pick_list("NETWORK", FILES, n);
            if (i < 0) continue;
            strcpy(line, "net join "); strcat(line, FILES[i].name);
            if (ui_cmd(line, &body) != 0) continue;
            if (input_line("PSK: ", line + 8, 63) >= 0) { memmove(line, "net psk ", 8); ui_cmd(line, &body); }
            ui_dirty = 1;
        } else if (k == 'P') {
            if (input_line("PSK: ", line + 8, 63) >= 0) { memmove(line, "net psk ", 8); if (ui_cmd(line, &body) == 0) ui_dirty = 1; }
        } else if (k == 'H') {
            char port[6];
            strcpy(line, "net server ");
            if (input_line("HOST: ", line + 11, 63) <= 0) continue;
            if (input_line("PORT (65504): ", port, 5) > 0) { strcat(line, " "); strcat(line, port); }
            if (ui_cmd(line, &body) == 0) ui_dirty = 1;
        } else if (k == 'M') {
            if (ui_cmd(strcmp(bm, "net") == 0 ? "net mode native" : "net mode net", &body) == 0) ui_dirty = 1;
        } else if (k == 'V') do_save();
    }
}

void settings_run(void)
{
    char now[40], next[40], hdb[8], tv[16], clk[8], ts[20], *body;
    u32 t;
    u8 k;
    for (;;) {
        now[0] = next[0] = hdb[0] = tv[0] = clk[0] = '\0';
        if (ui_cmd("status", &body) != 0) return;
        line_value(body, "rom now ", now, sizeof now);
        line_value(body, "rom next ", next, sizeof next);
        line_value(body, "dw hdbdos ", hdb, sizeof hdb);
        if (ui_cmd("time", &body) != 0) return;
        line_value(body, "time ", tv, sizeof tv);
        line_value(body, "clock ", clk, sizeof clk);
        strip_load(now); strip_load(next);
        fmt_time(dec_to_u32(tv), ts);
        clear_screen();
        put_at(0, 0, "SETTINGS", 0);
        put_at(0, (u8)(COLS - 3 - strlen(fw)), "FW ", 0);
        put_at(0, (u8)(COLS - strlen(fw)), fw, 0);
        put_at(2, 0, "ROM NOW  ", 0);  put_at(2, 9, now, 0);
        put_at(3, 0, "ROM NEXT ", 0);  put_at(3, 9, next, 0);
        put_at(5, 0, "R:CHOOSE ROM FOR NEXT BOOT", 0);
        put_at(6, 0, "H:HDB-DOS DRIVE MODE ", 0); put_at(6, 21, hdb, 0);
        put_at(7, 0, "T:CLOCK ", 0);   put_at(7, 8, ts, 0);
        if (strcmp(clk, "kept") != 0) put_at(8, 2, "(LOST AT RESET)", 0);
        put_at(9, 0, "W:WIFI", 0);
        put_at(10, 0, "V:SAVE   BREAK:BACK", 0);
        k = key();
        if (k >= 'a' && k <= 'z') k = (u8)(k - 32);
        if (k == 3) return;
        if (k == 'R') pick_rom();
        else if (k == 'H') {
            if (ui_cmd(strcmp(hdb, "on") == 0 ? "dw hdbdos off" : "dw hdbdos on", &body) == 0) ui_dirty = 1;
        } else if (k == 'T') {
            put_at(12, 0, "FORMAT: YYYY-MM-DD HH:MM", 0);
            if (input_line("TIME: ", ts, 16) > 0) {
                if (parse_datetime(ts, &t) != 0) msg("BAD DATE/TIME");
                else {
                    strcpy(line, "time set ");
                    u32_to_dec(t, line + 9);
                    ui_cmd(line, &body);
                }
            }
        } else if (k == 'W') net_screen();
        else if (k == 'V') do_save();
    }
}
