#include "sim_bus.h"
#include "bus.h"
#include "mode.h"
#include "plat.h"

uint8_t sim_read(uint16_t addr) {
    uint16_t idx = addr & 0x3FFF;
    uint8_t v = bus_table[idx];
    bus_on_read_done(idx, plat_now_us());
    return v;
}

void sim_write(uint16_t addr, uint8_t d) {
    bus_on_write(addr & 0x3FFF, d, plat_now_us());
}

void sim_becker_putc(dw_server *dw, uint8_t b) {
    sim_write(0xFF42, b);
    mode_pump(dw, plat_now_ms());
}

int sim_becker_getc(dw_server *dw, int max_polls) {
    for (int i = 0; i < max_polls; i++) {
        mode_pump(dw, plat_now_ms());
        if (sim_read(0xFF41) & 2) return sim_read(0xFF42);
    }
    return -1;
}
