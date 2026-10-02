#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BUS_TABLE_SIZE 16384
#define BUS_IDX_BECKER_STATUS 0x3F41
#define BUS_IDX_BECKER_DATA   0x3F42
#define BUS_IDX_BECKER_CTL    0x3F43   /* $FF43, write-only: UI session control (src/ui) */
#define BUS_TRACE_SIZE 512    /* trace ring: the last 512 events, one word each */
#define BUS_MAX_HOOKS 4

/* functions only; static data is in SRAM regardless */
#ifdef PICOCO_HOST
#define BUS_HOT
#else
#define BUS_HOT __attribute__((section(".time_critical.bus")))   /* SRAM, see Plan B */
#endif

/* Event stream: the event SM (bus_engine.pio) samples GP0..GP22 once per
 * selected cycle, at its end, and DMA C writes that word into bus_events, a
 * ring core1 walks (bus_core1.c). Word: [7:0] D0-D7 (on a read, what the
 * engine drove), [21:8] A0-A13 = table index, [22] R/W (1 = read); the SM
 * reads 23 pins, so [31:23] are always 0. */
#define BUS_EVENTS 2048
extern volatile uint32_t bus_events[BUS_EVENTS];   /* Pico: 8 KB aligned (DMA C's write ring), linker region bus_ram */
#define BUS_EV_DATA(w) ((uint8_t)(w))
#define BUS_EV_IDX(w)  ((uint16_t)(((w) >> 8) & 0x3FFF))
#define BUS_EV_RD(w)   (((w) >> 22) & 1u)
#define BUS_EV(idx, rd, data) ((uint32_t)((data) & 0xFF) | (uint32_t)((idx) & 0x3FFF) << 8 | ((rd) ? 1u << 22 : 0u))
/* An empty ring slot. Never a real event: bits 23-31 read 0, and it would be
 * index 0x3FFF ($FFFF), which is never selected (the selects cover indices
 * 0x0000-0x3EFF and 0x3F40-0x3F5F). core1 writes it back into every slot it
 * has consumed. */
#define BUS_EV_NONE 0xFFFFFFFFu
void bus_event(uint32_t w);                    /* BUS_HOT: one cycle: trace, counters, hooks, write queue */

typedef struct { uint32_t seq; uint16_t idx; uint8_t rw; uint8_t data; } bus_trace_entry;   /* seq: events since bus_init */
typedef struct {
    uint32_t cycles, reads, writes, write_overrun;
    uint32_t whooks_run;     /* write hooks executed on core1 */
    uint32_t engine_stall;   /* engine restarts by the stall guard (Task 4) */
    uint32_t event_lag_max;  /* core1: most ring entries waiting, sampled every 256 events */
    uint32_t event_drop;     /* core0: checks that found the event SM had dropped a push (RX FIFO full) */
    uint32_t event_lap;      /* core1: lag checks that found the ring lapped (events overwritten unread) */
    uint32_t start_wait_cap; /* core0, Plus-W: engine starts whose wait for OE_BUS high hit its cap */
} bus_stats_t;

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
void bus_io_set(uint16_t idx, uint8_t v);             /* BUS_HOT: stores into all BUS_BANKS banks; ignores idx outside BUS_IO_LO..HI */

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
/* Trace: bus_event stores every event into a 512-word ring until frozen.
 * Frozen (here, by `trace freeze`, or by a core1 fault hook) it keeps the
 * events up to the freeze; `trace run` (freeze false) starts it afresh. */
void bus_trace_freeze(bool freeze);
bool bus_trace_frozen(void);
void bus_trace_freeze_hot(void);              /* BUS_HOT; freezes (never thaws); the current event is kept */
size_t bus_trace_copy(bus_trace_entry *out, size_t max);            /* the last max events, oldest first */
