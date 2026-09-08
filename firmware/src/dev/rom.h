#pragma once
#include <stdint.h>
#include <stddef.h>
#include "dw_store.h"

void rom_init(void);      /* registers device "rom" 0x0000..0x3EFF, no on_write */
void rom_pattern(void);   /* bus_table[i] = i & 0xFF for i < 0x2000 */
void rom_off(void);       /* 0xFF for 0x0000..0x3EFF */
int  rom_load_mem(const uint8_t *p, size_t n);        /* 8192 or 16384 only */
int  rom_load_file(dw_store *st, const char *name);   /* size must be 8192 or 16384; -1 not found, -2 bad size */
