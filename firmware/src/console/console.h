#pragma once
#include <stdint.h>
#include <stddef.h>
#include "dw.h"
#include "dw_store.h"

typedef void (*console_out_fn)(void *ctx, const char *s);

void console_init(console_out_fn out, void *ctx, dw_server *dw, dw_store *store);
void console_feed(const uint8_t *buf, size_t n);   /* accumulates up to 127 chars; executes on \r or \n */
int  console_exec(const char *line);               /* 0 ok (printed "ok"), -1 err (printed "err <msg>") */
int  console_run_config(void);                      /* plat_cfg_read, exec each line; lines executed, or <0 */
