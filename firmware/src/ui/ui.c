#include "ui.h"
#include "plat.h"
#include "rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_ROMS  64
#define NAME_LEN  32            /* longest name kept, with its NUL */
#define LIST_ROWS 12
#define POLL_QUIET_MS 250       /* a poll cut short is dropped after this; the stub retries at ~1.3 s */

static ui_send_fn g_send;
static void *g_ctx;
static ui_exec_fn g_exec;
static dw_store *g_store;
static bool active;

static uint8_t scr[UI_CELLS];   /* what the screen should show, VDG screen codes */
static uint8_t sent[UI_CELLS];  /* what the last reply leaves on the CoCo */
static uint8_t ack[UI_CELLS];   /* what the CoCo is known to show */
static uint8_t reply[UI_REPLY_MAX + 4];
static size_t reply_len;

static uint8_t caps;
static char roms[MAX_ROMS][NAME_LEN];
static int nroms, sel, top;
static bool truncated;
static char msg[UI_COLS + 1];
static char pending[NAME_LEN];
static uint8_t pending_act;

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

void ui_row_text(int row, char out[UI_COLS + 1]) {
    for (int c = 0; c < UI_COLS; c++) {
        uint8_t s = scr[row * UI_COLS + c];
        if (s < 0x20) s += 0x40;            /* inverse letters */
        else if (s >= 0x60) s -= 0x40;      /* normal punctuation and digits */
        out[c] = (char)s;                   /* $20-$3F inverse digits, $40-$5F normal letters: as is */
    }
    out[UI_COLS] = '\0';
}

static void rom_cb(const char *name, uint32_t size, void *ctx) {
    (void)size; (void)ctx;
    size_t l = strlen(name);
    /* `rom load` takes one token, so a name with a space cannot be sent. */
    if (l < 5 || l >= NAME_LEN || strcasecmp(name + l - 4, ".rom") != 0 || strchr(name, ' ')) return;
    if (nroms >= MAX_ROMS) { truncated = true; return; }
    memcpy(roms[nroms++], name, l + 1);
}

static int name_cmp(const void *a, const void *b) { return strcasecmp(a, b); }

static void load_list(void) {
    nroms = 0;
    truncated = false;
    plat_fs_list(rom_cb, NULL);
    qsort(roms, (size_t)nroms, NAME_LEN, name_cmp);
    sel = top = 0;
}

static void draw(void) {
    memset(scr, 0x60, sizeof scr);
    char line[UI_COLS + 1];
    snprintf(line, sizeof line, "PICOCO  %s %s%s",
             caps & UI_CAP_64K ? "64K" : caps & UI_CAP_32K ? "32K" : "16K",
             caps & UI_CAP_ECB ? "ECB" : "NO ECB",
             caps & UI_CAP_COCO3 ? " COCO3" : "");
    put(0, 0, line, false);
    put(1, 0, nroms ? "ROMS" : "NO .ROM FILES", false);
    for (int i = 0; i < LIST_ROWS && top + i < nroms; i++) put(2 + i, 1, roms[top + i], top + i == sel);
    put(14, 0, msg[0] ? msg : truncated ? "LIST TRUNCATED" : "", false);
    put(15, 0, "ENTER=RUN  BREAK=BASIC", false);
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

static uint8_t on_key(uint8_t key);   /* Task 2 */

static void on_poll(uint8_t flags, uint8_t key, uint8_t c) {
    if ((flags & 1) && reply_len) { g_send(g_ctx, reply, reply_len); return; }
    bool first = (flags & 2) != 0;
    uint8_t act = 0;
    if (first) { caps = c; load_list(); }
    else { memcpy(ack, sent, sizeof ack); act = on_key(key); }   /* a new poll acknowledges the last reply */
    draw();
    build_reply(first, act);
}

static void on_go(void);              /* Task 2 */

void ui_feed(const uint8_t *buf, size_t n, uint32_t now_ms) {
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

void ui_init(ui_send_fn send, void *ctx, ui_exec_fn exec, dw_store *store) {
    g_send = send; g_ctx = ctx; g_exec = exec; g_store = store;
    active = false;
    pkn = 0; reply_len = 0; pending_act = 0;
    nroms = sel = top = 0; truncated = false;
    msg[0] = '\0'; pending[0] = '\0'; caps = 0;
    memset(scr, 0x60, sizeof scr);
    memset(sent, 0x60, sizeof sent);
    memset(ack, 0x60, sizeof ack);
}

/* Task 2 replaces these two. */
static uint8_t on_key(uint8_t key) { (void)key; return 0; }
static void on_go(void) { }
