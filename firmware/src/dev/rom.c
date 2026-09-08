#include "rom.h"
#include "device.h"
#include "bus.h"
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
    if (n == 8192) {
        memcpy(&bus_table[0], p, n);
        return 0;
    }
    /* 16 K load: bus_table[0x3F41]/[0x3F42] are becker's, not ROM's, and only
     * core1's read hooks may write them (single-writer rule, see becker.c).
     * Copy around those two indices instead of overwriting-then-relying-on
     * the next $FF41 poll to restore them — no window where a core1 read
     * would see a transient ROM byte there. */
    memcpy(&bus_table[0], p, 0x3F41);
    memcpy(&bus_table[0x3F43], p + 0x3F43, n - 0x3F43);
    return 0;
}

/* ponytail: 16 KB staging buffer; could read straight into bus_table since
 * size is validated first. */
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
