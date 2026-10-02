#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dw_store.h"

/* On-board manager UI (spec 2026-10-01-rom-manager-design.md). core0 only.
 * The CoCo stub polls with the last key; ui answers with an action list. */
#define UI_COLS 32
#define UI_ROWS 16
#define UI_CELLS (UI_COLS * UI_ROWS)
#define UI_REPLY_MAX 600            /* action bytes in one reply; the stub's buffer */

#define UI_CAP_32K   0x01
#define UI_CAP_64K   0x02
#define UI_CAP_ECB   0x04
#define UI_CAP_DOS   0x08
#define UI_CAP_COCO3 0x10
#define UI_CAP_MEM3_MASK 0xC0       /* CoCo 3 RAM, only with UI_CAP_COCO3: 00 128K, 01 512K, 10 1 MB, 11 2 MB */

#define UI_ACT_END   0x00
#define UI_ACT_TEXT  0x01           /* offset hi, lo, length, screen codes */
#define UI_ACT_CLEAR 0x02
#define UI_ACT_JUMP  0x10           /* after a ROM swap: JMP $C000 */
#define UI_ACT_COLD  0x11           /* after a ROM swap: cold restart */
#define UI_ACT_WARM  0x13           /* warm restart: back to BASIC */
#define UI_ACT_JUMP3 0x16           /* CoCo 3, after a ROM swap: slow speed, $CC -> $FF90, ROM mode, JMP $C000 */
#define UI_ACT_COLD3 0x17           /* CoCo 3, after a ROM swap: slow speed, clear $FEED and $71, JMP $8C1B (recopies the cart to RAM) */
#define UI_ACT_WARM3 0x18           /* CoCo 3: JMP $8C1B with $71 untouched */

#define UI_KEY_BREAK 0x03
#define UI_KEY_DOWN  0x0A
#define UI_KEY_ENTER 0x0D
#define UI_KEY_UP    0x5E

#define UI_GO_OK   0x06             /* reply to the stub's 'G' */
#define UI_GO_FAIL 0x15

typedef void (*ui_send_fn)(void *ctx, const uint8_t *buf, size_t n);
typedef int  (*ui_exec_fn)(const char *line, char *msg, size_t cap);   /* 0 ok, else msg = reason */

void ui_init(ui_send_fn send, void *ctx, ui_exec_fn exec, dw_store *store);
void ui_ctl(uint8_t code);          /* a write to $FF43: 0xA5 begin, 0x5A end */
bool ui_active(void);
void ui_feed(const uint8_t *buf, size_t n, uint32_t now_ms);   /* bytes from the CoCo during a session */
void ui_row_text(int row, char out[UI_COLS + 1]);              /* tests: one model row as ASCII */
