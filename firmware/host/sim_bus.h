#pragma once
#include <stdint.h>
#include "dw.h"

/* Virtual CoCo: drives bus.h the way the engine and core1 event loop do,
 * so the host build can exercise the whole stack end to end. Addresses are
 * full 16-bit CoCo addresses ($C000..$FFFF); only the low 14 bits index
 * bus_table (see BUS_TABLE_SIZE). */
uint8_t sim_read(uint16_t addr);
void    sim_write(uint16_t addr, uint8_t d);

/* HDB-DOS style helpers built on sim_read/sim_write + mode_pump. */
int  sim_becker_getc(dw_server *dw, int max_polls);   /* byte or -1 on stall */
void sim_becker_putc(dw_server *dw, uint8_t b);
