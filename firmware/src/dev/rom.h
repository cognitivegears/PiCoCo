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
/* -1 not found/read error, -2 bad size. A read error on a banked (32/64/128 K)
 * file can happen after rom_banks_begin() has already unbanked the ROM and
 * partially overwritten rom_banks; on that path the ROM is simply left off
 * (rom_loaded() false), not restored to whatever was loaded before. */
int  rom_load_file(dw_store *st, const char *name);
int  rom_bank_count(void);   /* 0 = unbanked */
bool rom_loaded(void);       /* true after a successful load until rom_off/rom_pattern */
bool rom_is_dos(void);       /* loaded and bytes 0,1 == "DK" (HDB-DOS / RS-DOS style ROM) */

/* Bank-fill primitives: build a banked image straight into rom_banks with no
 * separate staging buffer (used by rom_load_file's banked path and by
 * fake6809.c's synthetic self-test image). */
void     rom_banks_begin(void);      /* unbank: count 0, base = bus_table, rom_have/rom_dos false */
uint8_t *rom_bank_buf(int b);        /* pointer to rom_banks[b]; NULL if b >= ROM_MAX_BANKS */
int      rom_publish_banks(int nb);  /* nb must be 2, 4 or 8 (else -2); sets base = bank 0, mask,
                                       * count, rom_have, rom_dos (from bank 0 bytes 0-1); 0 ok */

typedef enum { CART_AUTO = 0, CART_ON, CART_OFF } cart_mode_t;
void        rom_cart_set(cart_mode_t m);
cart_mode_t rom_cart_get(void);
bool        rom_cart_wanted(void);   /* pulse /CART after reset? ON: yes; OFF: no; AUTO: a non-DOS ROM is loaded */
