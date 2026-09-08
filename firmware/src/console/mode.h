#pragma once
#include <stdint.h>
#include <stddef.h>
#include "dw.h"

typedef enum { MODE_DIAG, MODE_LOOP, MODE_BRIDGE, MODE_NATIVE } picoco_mode;

typedef struct { uint32_t reply_overflow; } mode_stats_t;
extern mode_stats_t mode_stats;

void mode_bind(dw_server *dw);               /* console_init calls this once */
void mode_reset(void);                        /* tests: back to MODE_DIAG, clears pending reply FIFO */
void mode_set(picoco_mode m);                 /* also resets dw parser to IDLE on entering NATIVE */
picoco_mode mode_get(void);
const char *mode_name(picoco_mode m);         /* "off","loop","bridge","native" (console words) */
void mode_pump(dw_server *dw, uint32_t now_ms);   /* one iteration: dispatch writes; per-mode pump */

/* dw_init's send fn in NATIVE mode: queues dw's reply bytes into a
 * 4096-byte pending FIFO (see mode.c for the bound) that mode_pump drains
 * into becker_write as tx space frees up
 * (a DriveWire reply can be up to 259 bytes, more than becker's 255-byte
 * to_coco queue holds at once). */
void mode_dw_send(void *ctx, const uint8_t *buf, size_t n);
