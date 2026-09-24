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

u8 ui_dirty;
static char reply[768];
static char lsbuf[4120];   /* firmware body up to 4096 + 23-byte OK header + NUL */
static file_ent files[MAX_FILES];
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

void msg(const char *s)
{
    clear_row(MSG_ROW);
    put_at(MSG_ROW, 0, s, 1);
    key();
}

int ui_cmd(const char *l, char **body)
{
    int rc = picoco_cmd(l, reply, sizeof reply, body);
    if (rc == PC_TIMEOUT) msg("PICOCO NOT RESPONDING");
    else if (rc == PC_BAD) msg("BAD REPLY (FIRMWARE >= 1.2?)");
    else if (rc == PC_TOOLONG) msg("NAME TOO LONG");
    else if (rc > 0) msg(*body);
    return rc;
}

int input_line(const char *prompt, char *buf, int max)
{
    int n = 0;
    u8 k, col = (u8)strlen(prompt);
    clear_row(MSG_ROW); clear_row(MSG_ROW + 1);
    put_at(MSG_ROW, 0, prompt, 0);
    for (;;) {
        buf[n] = '\0';
        memset(VRAM + MSG_ROW * COLS + col, 0x60, COLS - col);
        put_at(MSG_ROW, col, buf, 0);
        if (col + n < COLS) VRAM[MSG_ROW * COLS + col + n] = 0x20;   /* inverse-space cursor */
        k = key();
        if (k == 13) return n;
        if (k == 3) return -1;
        if (k == 8) { if (n) n--; continue; }
        if (k >= 32 && k < 127 && n < max && col + n < COLS - 1) buf[n++] = (char)k;
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
    }
    VRAM[(u16)row * COLS + 26] = inv ? 0x20 : 0x60;   /* keep a gap before the size */
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

static void load_files(void)
{
    char *body;
    nfiles = 0;
    if (picoco_cmd("fs ls", lsbuf, sizeof lsbuf, &body) != 0) { msg("CANNOT LIST FILES"); return; }
    nfiles = parse_ls(body, files, MAX_FILES, 0);
    sort_files(files, nfiles);
    if (sel >= nfiles) sel = nfiles ? nfiles - 1 : 0;
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
    put_at(14, 0, "0-3:MOUNT SHIFT+ E:EJECT B:BOOT", 0);
    put_at(15, 0, "N:NEW S:SET V:SAVE BREAK:EXIT", 0);
}

static void draw_all(void)
{
    clear_screen();
    draw_header();
    top = clamp_top(sel, top, LIST_ROWS);
    if (nfiles) draw_list(files, nfiles, sel, top, LIST_TOP, LIST_ROWS);
    else put_at(LIST_TOP, 1, "NO DISK IMAGES ON FLASH", 0);
    draw_help();
}

static void do_mount(u8 d)
{
    char *body;
    if (!nfiles) return;
    strcpy(line, "dw disk insert ");
    line[15] = (char)('0' + d); line[16] = ' '; line[17] = '\0';
    strcat(line, files[sel].name);
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
    for (i = 0; i < nfiles; i++) if (ends_with_ci(files[i].name, name) && strlen(files[i].name) == strlen(name)) sel = i;
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
        if (strlen(files[i].name) >= (u16)l) {
            char save = files[i].name[l];
            int hit;
            files[i].name[l] = '\0';
            hit = ends_with_ci(files[i].name, jump);   /* whole prefix, case-insensitive */
            files[i].name[l] = save;
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
    if (k == 'V' || k == 'v') { do_save(); return 1; }
    return k == 3;
}

void ui_run(void)
{
    char *body;
    u8 k;
    clear_screen();
    put_at(0, 0, "PICOCO MANAGER", 0);
    put_at(2, 0, "CONNECTING...", 0);
    if (ui_cmd("version", &body) != 0) return;
    line_value(body, "version ", fw, sizeof fw);
    load_drives();
    load_files();
    for (;;) {
        draw_all();
        k = key();
        if (k == 3) { if (confirm_exit()) { clear_screen(); return; } continue; }
        if (k == 94 && sel > 0) sel--;
        else if (k == 10 && sel < nfiles - 1) sel++;
        else if (k == 95) sel = sel > LIST_ROWS ? sel - LIST_ROWS : 0;           /* SHIFT+UP */
        else if (k == 91) sel = sel + LIST_ROWS < nfiles ? sel + LIST_ROWS : (nfiles ? nfiles - 1 : 0); /* SHIFT+DOWN */
        else if (k >= '0' && k <= '3') do_mount((u8)(k - '0'));
        else if (k == 'e') do_eject();
        else if (k == 'n') do_new();
        else if (k == 'v') do_save();
        else if (k == 's') { settings_run(); load_drives(); load_files(); }
        else if (k == 'b' && nfiles) { if (boot_image(files[sel].name)) return; load_drives(); }
        else if (k >= 'A' && k <= 'Z') do_jump((char)k);
    }
}

void settings_run(void) { msg("SETTINGS: TASK 13"); }   /* replaced in Task 13 */
