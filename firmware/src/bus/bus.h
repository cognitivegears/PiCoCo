#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BUS_TABLE_SIZE 16384
#define BUS_IDX_BECKER_STATUS 0x3F41
#define BUS_IDX_BECKER_DATA   0x3F42
#define BUS_IDX_BECKER_CTL    0x3F43   /* $FF43, write-only: UI session control (src/ui) */
#define BUS_TRACE_SIZE 4096
#define BUS_MAX_HOOKS 4

/* functions only; static data is in SRAM regardless */
#ifdef PICOCO_HOST
#define BUS_HOT
#else
#define BUS_HOT __attribute__((section(".time_critical.bus")))   /* SRAM, see Plan B */
#endif

typedef struct { uint32_t t_us; uint16_t idx; uint8_t rw; uint8_t data; } bus_trace_entry;
typedef struct {
    uint32_t cycles, reads, writes, write_overrun, addr_resample, addr_resample_bits;
    uint32_t resample_key, resample_in;   /* last addr_resample: precomputed key and the OE-low gpio sample */
    uint32_t oe_glitch;       /* Pico 2 loop: OE_BUS sampled high once mid-read, then low again (see bus_core1.c) */
    uint32_t late_precompute; /* Pico 2 loop: cycle began before an idle sample was taken (back-to-back reads); served by the redrive path, benign */
    uint32_t whooks_run;     /* write hooks executed on core1 */
} bus_stats_t;   /* ponytail: addr_resample* = diagnostic, address changed between two samples after OE_BUS fell */

/* The table the engine serves: eight 16 KB banks in one 128 KB-aligned block.
 * The read SM builds a byte's address as &bus_mem | bank << 14 | A13..A0
 * (bus_engine.pio). Bank 0 is the unbanked table (bus_table); a banked image
 * fills banks 0..n-1. The I/O page ($FF00-$FFFF, idx 0x3F00-0x3FFF) comes
 * from the current bank too, so the entries devices own (BUS_IO_LO..HI) are
 * mirrored in every bank: write them only through bus_io_set, never from a
 * ROM loader. */
#define BUS_BANKS 8
#define BUS_IO_LO 0x3F40           /* device-owned entries, mirrored in every bank */
#define BUS_IO_HI 0x3F5F
extern uint8_t bus_mem[BUS_BANKS][BUS_TABLE_SIZE];   /* aligned to 128 KB */
#define bus_table (bus_mem[0])
extern volatile uint8_t bus_bank;                     /* current bank, 0 when unbanked */

/* What a CoCo read of idx returns. always_inline: used by core1. */
static inline __attribute__((always_inline)) uint8_t bus_peek(uint16_t idx) {
    return bus_mem[bus_bank][idx];
}
void bus_io_set(uint16_t idx, uint8_t v);             /* BUS_HOT: stores into all BUS_BANKS banks */

extern volatile bus_stats_t bus_stats;
extern volatile bool bus_drive;   /* false = never drive D0..D7 (capture-only, milestone 0.4); read by core1 each cycle */

void bus_drive_set(bool on);
bool bus_drive_get(void);
void bus_core1_main(void);                              /* BUS_HOT; never returns; core1 entry (launched from main.c) */

void bus_init(void);                                   /* table = 0xFF, rings empty, trace running */
void bus_set_read(uint16_t idx, uint8_t v);
void bus_set_read_range(uint16_t idx, const uint8_t *p, size_t n);   /* clipped at table end */
/* Both hook tables must be fully registered before multicore_launch_core1():
 * there is no publish barrier, so core1 (which reads hook_count/whook_count
 * with no synchronization) assumes the table is already settled by the time
 * it starts. Today every registration happens in a device's *_init(), all
 * called from main() before the launch. */
int  bus_add_read_hook(uint16_t idx, void (*fn)(void));             /* 0 ok, -1 full */
int  bus_add_write_hook(uint16_t idx, void (*fn)(uint8_t data));   /* 0 ok, -1 full; fn is BUS_HOT, runs on core1 before the event is queued */
bool bus_pop_write(uint16_t *idx, uint8_t *data);                   /* core0 consumer */
void bus_trace_freeze(bool freeze);
void bus_trace_freeze_hot(void);              /* BUS_HOT; freezes (never thaws) */
size_t bus_trace_copy(bus_trace_entry *out, size_t max);            /* oldest first, newest last */

/* producer side (core1 loop and sim_bus): */
void bus_on_read_done(uint16_t idx, uint32_t t_us);   /* runs hook for idx, traces (rw=1, data=bus_peek(idx)), stats */
void bus_run_read_hooks(uint16_t idx);                           /* BUS_HOT; first half of bus_on_read_done */
void bus_record_read(uint16_t idx, uint8_t data, uint32_t t_us);  /* BUS_HOT; second half: trace + counters */
void bus_on_write(uint16_t idx, uint8_t data, uint32_t t_us);   /* pushes write event, traces, stats */
