#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The PIO + DMA read engine (bus_engine.c, bus_engine.pio). core0 owns start
 * and stop; bus_engine_set_bank is safe from either core. */
void bus_engine_drive(bool on);          /* core0: start/stop the read path (bus_drive) */
void bus_engine_set_bank(uint8_t bank);  /* BUS_HOT, core1 or core0: bank for the next cycle */
void bus_engine_resources(void (*line)(const char *s));
