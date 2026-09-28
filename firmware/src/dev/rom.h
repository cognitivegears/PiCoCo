#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dw_store.h"

#define ROM_BANK_SIZE 16384
#define ROM_MAX_BANKS 8          /* 128 KB: the largest Games Master Cartridge image (MAME coco_gmc) */

void rom_init(void);      /* registers device "rom" 0x0000..0x3EFF and the $FF40 bank-select write hook */
void rom_pattern(void);   /* bus_table[i] = i & 0xFF for i < 0x2000; unbanked */
void rom_off(void);       /* 0xFF for 0x0000..0x3EFF; unbanked; rom_loaded() false */
/* 8192 or 16384: copied into bus_table as before. 32768/65536/131072: 16 KB
 * banks selected by a write to $FF40 (value & (banks-1)). Returns -2 for any
 * other size and leaves the current ROM in place. */
int  rom_load_mem(const uint8_t *p, size_t n);
int  rom_load_file(dw_store *st, const char *name);   /* -1 not found/read error, -2 bad size */
int  rom_bank_count(void);   /* 0 = unbanked */
bool rom_loaded(void);       /* true after a successful load until rom_off/rom_pattern */
bool rom_is_dos(void);       /* loaded and bytes 0,1 == "DK" (HDB-DOS / RS-DOS style ROM) */
