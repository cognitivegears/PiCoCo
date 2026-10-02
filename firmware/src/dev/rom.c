#include "rom.h"
#include "device.h"
#include "bus.h"
#include "bus_engine.h"
#include <string.h>

#define ROM_LO 0x0000
#define ROM_HI 0x3EFF

static const device_t rom_device = { "rom", ROM_LO, ROM_HI, NULL, NULL };

_Static_assert(ROM_MAX_BANKS == BUS_BANKS && ROM_BANK_SIZE == BUS_TABLE_SIZE, "ROM banks are bus_mem's banks");

/* Banks live in bus_mem; only the loaded ones are meaningful. */
static volatile uint8_t rom_nbanks;     /* 0 = unbanked; written by core0 (loader) only */
static volatile uint8_t rom_bank_mask;
static bool rom_have;
static bool rom_dos;
static cart_mode_t cart_mode;           /* initialized to CART_AUTO (0) by default */

/* core1: switch banks on a $FF40 write. Inert while unbanked, so it stays
 * registered for the life of the firmware (no remove API needed). */
static BUS_HOT void rom_bank_hook(uint8_t data) {
    if (rom_nbanks) { bus_bank = data & rom_bank_mask; bus_engine_set_bank(bus_bank); }
}

static void set_bank0(void) { bus_bank = 0; bus_engine_set_bank(0); }

/* n bytes of a ROM image into bank b, leaving the device-owned I/O entries
 * (BUS_IO_LO..HI) alone: only bus_io_set writes those. */
static void bank_copy(int b, const uint8_t *p, size_t n) {
    uint8_t *d = bus_mem[b];
    if (n <= BUS_IO_LO) { memcpy(d, p, n); return; }
    memcpy(d, p, BUS_IO_LO);
    if (n > BUS_IO_HI + 1) memcpy(d + BUS_IO_HI + 1, p + BUS_IO_HI + 1, n - (BUS_IO_HI + 1));
}

void rom_init(void) {
    cart_mode = CART_AUTO;
    rom_have = false;
    rom_dos = false;
    device_register(&rom_device);
    bus_add_write_hook(0x3F40, rom_bank_hook);
}

static void unbank(void) {
    rom_nbanks = 0;
    rom_bank_mask = 0;
    set_bank0();
}

void rom_banks_begin(void) {
    unbank();
    rom_have = false;
    rom_dos = false;
}

uint8_t *rom_bank_buf(int b) {
    if (b < 0 || b >= ROM_MAX_BANKS) return NULL;
    return bus_mem[b];
}

int rom_publish_banks(int nb) {
    if (nb != 2 && nb != 4 && nb != 8) return -2;
    /* Order matters for core1: bank 0, then mask, then the count that lets
     * the $FF40 hook start switching (same order as the old rom_load_mem
     * banked path). */
    set_bank0();
    rom_bank_mask = (uint8_t)(nb - 1);
    rom_nbanks = (uint8_t)nb;
    rom_have = true;
    rom_dos = (bus_mem[0][0] == 'D' && bus_mem[0][1] == 'K');
    return 0;
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
        /* Order matters for core1: stop the hook, fill banks, then publish
         * bank 0 and the count (rom_publish_banks). */
        rom_nbanks = 0;
        for (int b = 0; b < nb; b++) bank_copy(b, p + (size_t)b * ROM_BANK_SIZE, ROM_BANK_SIZE);
        rom_publish_banks(nb);   /* nb is always 2/4/8 here: bank_count_for only returns those */
    } else {
        unbank();
        bank_copy(0, p, n);      /* 16 K: the Becker entries are becker's (single-writer rule) */
        rom_have = true;
        rom_dos = (p[0] == 'D' && p[1] == 'K');
    }
    return 0;
}

static uint8_t rom_chunk_buf[ROM_BANK_SIZE];   /* staging: one bank at a time, so bank_copy can skip the I/O entries */

int rom_load_file(dw_store *st, const char *name) {
    dw_file f;
    if (st->ops->open(st->ctx, name, false, &f) < 0) return -1;
    uint32_t size;
    if (st->ops->size(&f, &size) < 0) { st->ops->close(&f); return -1; }
    int nb = bank_count_for(size);
    if (size != 8192 && size != 16384 && nb == 0) { st->ops->close(&f); return -2; }
    if (nb) {
        rom_banks_begin();
        for (int b = 0; b < nb; b++) {
            uint32_t got = 0;
            while (got < ROM_BANK_SIZE) {
                int r = st->ops->read(&f, (uint32_t)b * ROM_BANK_SIZE + got, rom_chunk_buf + got, ROM_BANK_SIZE - got);
                if (r <= 0) { st->ops->close(&f); rom_off(); return -1; }   /* ROM left off: no stale bytes in bus_table either */
                got += (uint32_t)r;
            }
            bank_copy(b, rom_chunk_buf, ROM_BANK_SIZE);
        }
        st->ops->close(&f);
        rom_publish_banks(nb);
        return 0;
    }
    uint32_t got = 0;
    while (got < size) {
        int r = st->ops->read(&f, got, rom_chunk_buf + got, size - got);
        if (r <= 0) { st->ops->close(&f); return -1; }
        got += (uint32_t)r;
    }
    st->ops->close(&f);
    return rom_load_mem(rom_chunk_buf, size);
}

int  rom_bank_count(void) { return rom_nbanks; }
bool rom_loaded(void)     { return rom_have; }
bool rom_is_dos(void)     { return rom_have && rom_dos; }

void        rom_cart_set(cart_mode_t m) { cart_mode = m; }
cart_mode_t rom_cart_get(void)          { return cart_mode; }
bool rom_cart_wanted(void) {
    if (cart_mode == CART_ON)  return true;
    if (cart_mode == CART_OFF) return false;
    return rom_have && !rom_dos;
}

int rom_check_file(dw_store *st, const char *name) {
    dw_file f;
    if (st->ops->open(st->ctx, name, false, &f) < 0) return -1;
    uint32_t size;
    int r = st->ops->size(&f, &size);
    st->ops->close(&f);
    if (r < 0) return -1;
    return (size == 8192 || size == 16384 || bank_count_for(size)) ? 0 : -2;   /* same sizes rom_load_mem takes */
}
