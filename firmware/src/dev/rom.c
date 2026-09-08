#include "rom.h"
#include "device.h"
#include "bus.h"
#include "becker.h"
#include <string.h>

#define ROM_LO 0x0000
#define ROM_HI 0x3EFF

static const device_t rom_device = { "rom", ROM_LO, ROM_HI, NULL, NULL };

void rom_init(void) {
    device_register(&rom_device);
}

void rom_pattern(void) {
    for (uint32_t i = 0; i < 0x2000; i++) bus_table[i] = (uint8_t)(i & 0xFF);
}

void rom_off(void) {
    memset(&bus_table[ROM_LO], 0xFF, ROM_HI - ROM_LO + 1);
}

int rom_load_mem(const uint8_t *p, size_t n) {
    if (n != 8192 && n != 16384) return -2;
    memcpy(&bus_table[0], p, n);
    if (n == 16384) becker_refresh();
    return 0;
}

static uint8_t rom_file_buf[16384];

int rom_load_file(dw_store *st, const char *name) {
    dw_file f;
    if (st->ops->open(st->ctx, name, false, &f) < 0) return -1;
    uint32_t size;
    if (st->ops->size(&f, &size) < 0) {
        st->ops->close(&f);
        return -1;
    }
    if (size != 8192 && size != 16384) {
        st->ops->close(&f);
        return -2;
    }
    int got = st->ops->read(&f, 0, rom_file_buf, size);
    st->ops->close(&f);
    if (got < 0 || (uint32_t)got != size) return -1;
    return rom_load_mem(rom_file_buf, size);
}
