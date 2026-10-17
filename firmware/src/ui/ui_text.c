#include "ui_text.h"
#include "ui.h"
#include <string.h>

#define ROW_PROMPT 12
#define ROW_TEXT   13           /* two rows of UI_COLS; the cursor takes one cell, so 63 characters fit */
#define ROW_FOOT   15           /* row 14 is the text's second row, so messages hide while editing */

static bool on, masked;
static char prompt[UI_COLS + 1];
static char buf[UI_TEXT_MAX + 1];
static char done[UI_TEXT_MAX + 1];
static int len, max_len;
static ui_text_cb cb;

void ui_text_begin(const char *p, const char *initial, bool mask, int max, ui_text_cb f) {
    strncpy(prompt, p, UI_COLS); prompt[UI_COLS] = '\0';
    max_len = max > UI_TEXT_MAX ? UI_TEXT_MAX : max;
    strncpy(buf, initial, (size_t)max_len); buf[max_len] = '\0';
    len = (int)strlen(buf);
    masked = mask; cb = f; on = true;
}

bool ui_text_active(void) { return on; }

bool ui_text_key(uint8_t key) {
    if (!on) return false;
    if (key == 0x08) { if (len) buf[--len] = '\0'; }
    else if (key == UI_KEY_BREAK) on = false;
    else if (key == UI_KEY_ENTER) {
        memcpy(done, buf, (size_t)len + 1);
        on = false;
        if (cb) cb(done);
    }
    else if (key >= 0x20 && key <= 0x7E && len < max_len) { buf[len++] = (char)key; buf[len] = '\0'; }
    return true;
}

/* The VDG has no lowercase: a-z show as the inverse capital ($01-$1A), the rest
 * as ui.c's code() would (normal video, $40-$5F as is, $20-$3F + $40). */
static uint8_t cell(char c) {
    uint8_t u = (uint8_t)c;
    if (u >= 'a' && u <= 'z') return (uint8_t)(u - 0x60);
    if (u >= 0x7B) return (uint8_t)(u - 0x20);      /* { | } ~ have no glyph of their own */
    if (u == 0x60) return 0x40;
    return u < 0x40 ? (uint8_t)(u + 0x40) : u;
}

static void put(int row, int col, const char *s) {
    for (; *s && col < UI_COLS; col++, s++) ui_set_cell(row, col, cell(*s));
}

void ui_text_draw(void) {
    if (!on) return;
    for (int r = ROW_PROMPT; r <= ROW_FOOT; r++) for (int c = 0; c < UI_COLS; c++) ui_set_cell(r, c, 0x60);
    put(ROW_PROMPT, 0, prompt);
    for (int i = 0; i < len; i++)
        ui_set_cell(ROW_TEXT + i / UI_COLS, i % UI_COLS, masked ? 0x6A : cell(buf[i]));
    ui_set_cell(ROW_TEXT + len / UI_COLS, len % UI_COLS, 0x20);   /* cursor: inverse space */
    put(ROW_FOOT, 0, "ENTER=OK  BREAK=CANCEL");
}
