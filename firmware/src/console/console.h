#pragma once
#include <stdint.h>
#include <stddef.h>
#include "dw.h"
#include "dw_store.h"
#include "mode.h"

typedef void (*console_out_fn)(void *ctx, const char *s);

void console_init(console_out_fn out, void *ctx, dw_server *dw, dw_store *store);
void console_feed(const uint8_t *buf, size_t n);   /* accumulates up to 127 chars; executes on \r or \n */
int  console_exec(const char *line);               /* 0 ok (printed "ok"), -1 err (printed "err <msg>") */
void console_set_boot_mode(picoco_mode m);         /* pending next-boot mode used by save */
int  console_run_config(void);                      /* plat_cfg_read, exec each line; lines executed, or <0 */

/* DriveWire vserial command handler (vser_exec_fn): allowlisted commands only,
 * output captured into out. 0 ok, else DW4 result code with out = message. */
int  console_exec_remote(void *ctx, const char *line, char *out, size_t cap, size_t *outn);
/* Run any console command with its output captured, for the on-board UI
 * (no allowlist: the caller is firmware). 0 ok; else msg = the text after "err ". */
int  console_exec_capture(const char *line, char *msg, size_t cap);
