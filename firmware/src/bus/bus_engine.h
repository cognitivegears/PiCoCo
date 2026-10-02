#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The PIO + DMA engine (bus_engine.c, bus_engine.pio). core0 owns start and
 * stop; the bank calls are safe from either core. */
void bus_engine_init(void);              /* core0, once, after net_init (cyw43 claims PIO2 first) and before core1: starts the event stream */
void bus_engine_drive(bool on);          /* core0: start/stop the read path (bus_drive) */
void bus_engine_tick(uint32_t now_ms);   /* core0 main loop: once a millisecond, the event-drop check */
void bus_engine_check_drops(void);       /* core0: count a drop the event SM flagged since the last check (bus_stats.event_drop) */

/* Bank for the next cycle; stores bus_bank under the engine's lock, so it and
 * the read SM's bank register never disagree. A caller whose decision depends
 * on other state (rom.c: the bank count) takes the lock itself and calls the
 * _locked form inside it. */
void bus_engine_set_bank(uint8_t bank);         /* BUS_HOT, either core */
uint32_t bus_engine_lock(void);                 /* BUS_HOT: interrupts off + the engine's spin lock */
void bus_engine_unlock(uint32_t saved);         /* BUS_HOT */
void bus_engine_set_bank_locked(uint8_t bank);  /* BUS_HOT: lock held */
void bus_engine_resources(void (*line)(const char *s));
