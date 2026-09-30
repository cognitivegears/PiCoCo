#include "test.h"
#include "bus.h"
#include "device.h"
#include "rom.h"
#include "becker.h"
#include "dw_store.h"
#include "sim_bus.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char g_dir[256];
static dw_store store;
static uint8_t img[ROM_MAX_BANKS * ROM_BANK_SIZE];

static void setup(void) {
    bus_init(); device_reset(); rom_init(); becker_init(); device_init_all();
}

/* bank b is filled with b, except byte 0 of bank 0 which is 'X' (not 'D'). */
static void fill_banks(int nbanks) {
    for (int b = 0; b < nbanks; b++)
        for (int i = 0; i < ROM_BANK_SIZE; i++) img[b * ROM_BANK_SIZE + i] = (uint8_t)b;
    img[0] = 'X';
}

static void write_file(const char *name, size_t n) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *fp = fopen(path, "wb");
    fwrite(img, 1, n, fp);
    fclose(fp);
}

TEST(small_loads_unchanged) {
    setup();
    fill_banks(1);
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT_EQ(rom_bank_count(), 0);
    ASSERT_EQ(sim_read(0xC000), 'X');
    ASSERT_EQ(sim_read(0xC001), 0);
    ASSERT(rom_loaded());
    ASSERT(!rom_is_dos());
    ASSERT_EQ(rom_load_mem(img, 16384), 0);
    ASSERT_EQ(rom_bank_count(), 0);
    ASSERT_EQ(bus_table[BUS_IDX_BECKER_STATUS], 0x00);   /* becker's entry untouched */
}

TEST(banked_switches_on_ff40_write) {
    setup();
    fill_banks(4);
    ASSERT_EQ(rom_load_mem(img, 4 * ROM_BANK_SIZE), 0);
    ASSERT_EQ(rom_bank_count(), 4);
    ASSERT_EQ(sim_read(0xC001), 0);
    sim_write(0xFF40, 2);
    ASSERT_EQ(sim_read(0xC001), 2);          /* visible on the very next read, no core0 drain */
    ASSERT_EQ(sim_read(0xFEFF), 2);
    sim_write(0xFF40, 9);                     /* 9 & 3 == 1: masked, never out of range */
    ASSERT_EQ(sim_read(0xC001), 1);
    ASSERT_EQ(bus_table[BUS_IDX_BECKER_STATUS], 0x00);   /* I/O page still comes from bus_table */
    ASSERT_EQ(bus_stats.whooks_run, 2);
}

TEST(rom_off_disables_banking) {
    setup();
    fill_banks(2);
    ASSERT_EQ(rom_load_mem(img, 2 * ROM_BANK_SIZE), 0);
    rom_off();
    ASSERT_EQ(rom_bank_count(), 0);
    ASSERT(!rom_loaded());
    ASSERT_EQ(sim_read(0xC001), 0xFF);
    sim_write(0xFF40, 1);
    ASSERT_EQ(sim_read(0xC001), 0xFF);       /* hook is inert when unbanked */
}

TEST(bad_sizes_refused_previous_kept) {
    setup();
    fill_banks(2);
    ASSERT_EQ(rom_load_mem(img, 2 * ROM_BANK_SIZE), 0);
    ASSERT_EQ(rom_load_mem(img, 24576), -2);
    ASSERT_EQ(rom_load_mem(img, 3 * ROM_BANK_SIZE), -2);
    ASSERT_EQ(rom_load_mem(img, 6 * ROM_BANK_SIZE), -2);
    ASSERT_EQ(rom_load_mem(img, 9 * ROM_BANK_SIZE), -2);
    ASSERT_EQ(rom_bank_count(), 2);
    sim_write(0xFF40, 1);
    ASSERT_EQ(sim_read(0xC001), 1);
}

TEST(dos_signature) {
    setup();
    fill_banks(1);
    img[0] = 'D'; img[1] = 'K';
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(rom_is_dos());
    rom_off();
    ASSERT(!rom_is_dos());
}

TEST(file_load_banked) {
    setup();
    fill_banks(8);
    write_file("big.rom", 8 * ROM_BANK_SIZE);
    write_file("odd.rom", 24576);
    ASSERT_EQ(rom_load_file(&store, "big.rom"), 0);
    ASSERT_EQ(rom_bank_count(), 8);
    sim_write(0xFF40, 7);
    ASSERT_EQ(sim_read(0xC001), 7);
    ASSERT_EQ(rom_load_file(&store, "odd.rom"), -2);
    ASSERT_EQ(rom_bank_count(), 8);
    ASSERT_EQ(rom_load_file(&store, "missing.rom"), -1);
}

TEST(cart_autostart_decision) {
    setup();
    ASSERT_EQ(rom_cart_get(), CART_AUTO);
    ASSERT(!rom_cart_wanted());                 /* no ROM */
    fill_banks(1);
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(rom_cart_wanted());                  /* plain pak */
    img[0] = 'D'; img[1] = 'K';
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(!rom_cart_wanted());                 /* DOS ROM: never pulse, it would jump to $C000 as code */
    rom_cart_set(CART_ON);
    ASSERT(rom_cart_wanted());
    rom_cart_set(CART_OFF);
    img[0] = 'X';
    ASSERT_EQ(rom_load_mem(img, 8192), 0);
    ASSERT(!rom_cart_wanted());
    rom_cart_set(CART_AUTO);
}

TEST(bank_buf_bounds) {
    setup();
    ASSERT_EQ(rom_publish_banks(3), -2);
    ASSERT(rom_bank_buf(ROM_MAX_BANKS) == NULL);
}

int main(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";
    snprintf(g_dir, sizeof(g_dir), "%s/picoco_test_rom_%d", tmpdir, (int)getpid());
    char cmd[300]; snprintf(cmd, sizeof(cmd), "mkdir -p %s", g_dir); system(cmd);
    dw_store_posix_init(&store, g_dir);
    RUN(small_loads_unchanged);
    RUN(banked_switches_on_ff40_write);
    RUN(rom_off_disables_banking);
    RUN(bad_sizes_refused_previous_kept);
    RUN(dos_signature);
    RUN(file_load_banked);
    RUN(cart_autostart_decision);
    RUN(bank_buf_bounds);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", g_dir); system(cmd);
    TEST_MAIN_END
}
