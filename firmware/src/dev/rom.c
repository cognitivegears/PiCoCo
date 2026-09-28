#include "rom.h"
#include "device.h"
#include "bus.h"
#include <string.h>

#define ROM_LO 0x0000
#define ROM_HI 0x3EFF

static const device_t rom_device = { "rom", ROM_LO, ROM_HI, NULL, NULL };

/* 128 KB of SRAM, .bss. Only the loaded banks are meaningful. */
static uint8_t rom_banks[ROM_MAX_BANKS][ROM_BANK_SIZE];
static volatile uint8_t rom_nbanks;     /* 0 = unbanked; written by core0 (loader) only */
static volatile uint8_t rom_bank_mask;
static bool rom_have;
static bool rom_dos;

/* core1: swap the ROM window on a $FF40 write. Inert while unbanked, so it
 * stays registered for the life of the firmware (no remove API needed). */
static BUS_HOT void rom_bank_hook(uint8_t data) {
    if (rom_nbanks) bus_rom_base = rom_banks[data & rom_bank_mask];
}

void rom_init(void) {
    device_register(&rom_device);
    bus_add_write_hook(0x3F40, rom_bank_hook);
}

static void unbank(void) {
    rom_nbanks = 0;
    rom_bank_mask = 0;
    bus_rom_base = bus_table;
}

void rom_pattern(void) {
    unbank();
    for (uint32_t i = 0; i < 0x2000; i++) bus_table[i] = (uint8_t)(i & 0xFF);
    rom_have = false; rom_dos = false;
}

void rom_off(void) {
    unbank();
    memset(&bus_table[ROM_LO], 0xFF, ROM_HI - ROM_LO + 1);
    rom_have = false; rom_dos = false;
}

static int bank_count_for(size_t n) {
    if (n == 2 * ROM_BANK_SIZE || n == 4 * ROM_BANK_SIZE || n == 8 * ROM_BANK_SIZE) return (int)(n / ROM_BANK_SIZE);
    return 0;
}

int rom_load_mem(const uint8_t *p, size_t n) {
    int nb = bank_count_for(n);
    if (n != 8192 && n != 16384 && nb == 0) return -2;
    if (nb) {
        /* Order matters for core1: fill banks, point the base at bank 0, then
         * publish the count so the hook can start switching. */
        rom_nbanks = 0;
        for (int b = 0; b < nb; b++) memcpy(rom_banks[b], p + (size_t)b * ROM_BANK_SIZE, ROM_BANK_SIZE);
        bus_rom_base = rom_banks[0];
        rom_bank_mask = (uint8_t)(nb - 1);
        rom_nbanks = (uint8_t)nb;
    } else {
        unbank();
        if (n == 8192) {
            memcpy(&bus_table[0], p, n);
        } else {
            /* 16 K load: bus_table[0x3F41]/[0x3F42] are becker's, not ROM's, and
             * only core1's read hooks may write them (single-writer rule). */
            memcpy(&bus_table[0], p, 0x3F41);
            memcpy(&bus_table[0x3F43], p + 0x3F43, n - 0x3F43);
        }
    }
    rom_have = true;
    rom_dos = (p[0] == 'D' && p[1] == 'K');
    return 0;
}

static uint8_t rom_file_buf[ROM_MAX_BANKS * ROM_BANK_SIZE];   /* ponytail: 128 KB staging; could read bank by bank into rom_banks */

int rom_load_file(dw_store *st, const char *name) {
    dw_file f;
    if (st->ops->open(st->ctx, name, false, &f) < 0) return -1;
    uint32_t size;
    if (st->ops->size(&f, &size) < 0) { st->ops->close(&f); return -1; }
    if (size != 8192 && size != 16384 && bank_count_for(size) == 0) { st->ops->close(&f); return -2; }
    uint32_t got = 0;
    while (got < size) {
        int r = st->ops->read(&f, got, rom_file_buf + got, size - got);
        if (r <= 0) { st->ops->close(&f); return -1; }
        got += (uint32_t)r;
    }
    st->ops->close(&f);
    return rom_load_mem(rom_file_buf, size);
}

int  rom_bank_count(void) { return rom_nbanks; }
bool rom_loaded(void)     { return rom_have; }
bool rom_is_dos(void)     { return rom_have && rom_dos; }
