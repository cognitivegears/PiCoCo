#include "bus.h"
#include "bus_engine.h"
#include <string.h>

#ifdef PICOCO_HOST
uint8_t bus_mem[BUS_BANKS][BUS_TABLE_SIZE] = { { 0 } };   /* no engine: no alignment; initialized so it is not a common symbol (ld64 + ASan warn on its alignment) */
volatile uint32_t bus_events[BUS_EVENTS];
#else
/* Both in the bus_ram linker region (firmware/ld/), NOLOAD so not zeroed:
 * bus_init fills bus_mem, engine_init fills bus_events. Their own region
 * keeps the 128 KB alignment from padding .bss. */
uint8_t bus_mem[BUS_BANKS][BUS_TABLE_SIZE] __attribute__((section(".bus_mem"), aligned(BUS_BANKS * BUS_TABLE_SIZE)));
volatile uint32_t bus_events[BUS_EVENTS] __attribute__((section(".bus_events"), aligned(BUS_EVENTS * 4)));
#endif
volatile uint8_t bus_bank;
volatile bus_stats_t bus_stats;
volatile bool bus_drive;

typedef struct { uint16_t idx; uint8_t data; } bus_write_ev_t;
#define WEV_SIZE 256
#define WEV_MASK (WEV_SIZE - 1)
static bus_write_ev_t wev[WEV_SIZE];
static volatile uint32_t wev_head, wev_tail;

typedef struct { uint16_t idx; void (*fn)(void); } bus_hook_t;
static bus_hook_t hooks[BUS_MAX_HOOKS];
static int hook_count;
static uint16_t hook_lo = 0xFFFF;   /* lowest hooked index */

typedef struct { uint16_t idx; void (*fn)(uint8_t); } bus_whook_t;
static bus_whook_t whooks[BUS_MAX_HOOKS];
static int whook_count;

/* Trace ring: event seq n sits in slot n % 512 as the event word with
 * (n >> 9) in bits 23-31, so a copy can tell an entry overwritten by a later
 * event (or never written) from the one it wants. Written by core1 only
 * (bus_event); start/end are moved by freeze and run. */
static volatile uint32_t trace_ring[BUS_TRACE_SIZE];
static volatile uint32_t trace_seq;     /* events seen since bus_init */
static volatile uint32_t trace_start;   /* first seq stored since the last `trace run` */
static volatile uint32_t trace_end;     /* frozen: seq after the last one stored */
static volatile bool trace_frozen;
#define TRACE_TAG(n) ((n) >> 9 << 23)

void bus_init(void) {
    memset(bus_mem, 0xFF, sizeof(bus_mem));
    bus_bank = 0;
    wev_head = 0;
    wev_tail = 0;
    hook_count = 0;
    hook_lo = 0xFFFF;
    whook_count = 0;
    for (int i = 0; i < BUS_TRACE_SIZE; i++) trace_ring[i] = BUS_EV_NONE;   /* tag 0x1FF: no early seq matches it */
    trace_seq = trace_start = trace_end = 0;
    trace_frozen = false;
    memset((void *)&bus_stats, 0, sizeof bus_stats);
    bus_drive = false;
}

#ifndef PICOCO_HOST
void bus_drive_set(bool on) { bus_drive = on; bus_engine_drive(on); }
#else
/* host: no engine to re-point, nothing to lock */
void bus_drive_set(bool on) { bus_drive = on; }
uint32_t bus_engine_lock(void) { return 0; }
void bus_engine_unlock(uint32_t saved) { (void)saved; }
void bus_engine_set_bank_locked(uint8_t bank) { bus_bank = bank & (BUS_BANKS - 1); }
void bus_engine_set_bank(uint8_t bank) { bus_engine_set_bank_locked(bank); }
#endif
bool bus_drive_get(void) { return bus_drive; }

BUS_HOT void bus_io_set(uint16_t idx, uint8_t v) {
    if (idx < BUS_IO_LO || idx > BUS_IO_HI) return;   /* only the device-owned entries are mirrored */
    for (int b = 0; b < BUS_BANKS; b++) bus_mem[b][idx] = v;
}

BUS_HOT void bus_set_read(uint16_t idx, uint8_t v) {
    if (idx >= BUS_TABLE_SIZE) return;
    if (idx >= BUS_IO_LO && idx <= BUS_IO_HI) bus_io_set(idx, v);
    else bus_table[idx] = v;
}

void bus_set_read_range(uint16_t idx, const uint8_t *p, size_t n) {
    if (idx >= BUS_TABLE_SIZE) return;
    size_t max = BUS_TABLE_SIZE - idx;
    size_t cnt = n < max ? n : max;
    for (size_t i = 0; i < cnt; i++) bus_set_read((uint16_t)(idx + i), p[i]);   /* I/O entries mirrored */
}

int bus_add_read_hook(uint16_t idx, void (*fn)(void)) {
    if (hook_count >= BUS_MAX_HOOKS) return -1;
    for (int i = 0; i < hook_count; i++) if (hooks[i].idx == idx) return -1;   /* dispatch stops at the first match */
    if (idx < hook_lo) hook_lo = idx;
    hooks[hook_count].idx = idx;
    hooks[hook_count].fn = fn;
    hook_count++;
    return 0;
}

int bus_add_write_hook(uint16_t idx, void (*fn)(uint8_t)) {
    if (whook_count >= BUS_MAX_HOOKS) return -1;
    whooks[whook_count].idx = idx;
    whooks[whook_count].fn = fn;
    whook_count++;
    return 0;
}

bool bus_pop_write(uint16_t *idx, uint8_t *data) {
    uint32_t t = wev_tail;
    if (__atomic_load_n(&wev_head, __ATOMIC_ACQUIRE) == t) return false;
    *idx = wev[t].idx;
    *data = wev[t].data;
    __atomic_store_n(&wev_tail, (t + 1) & WEV_MASK, __ATOMIC_RELEASE);
    return true;
}

void bus_trace_freeze(bool freeze) {
    if (freeze && !trace_frozen) {
        trace_frozen = true;                 /* first: an event stored after this has seq >= the end below */
        trace_end = trace_seq;
    } else if (!freeze && trace_frozen) {
        trace_start = trace_seq;             /* the events missed while frozen leave a gap: start afresh */
        trace_frozen = false;
    }
}

bool bus_trace_frozen(void) { return trace_frozen; }

/* core1, from a hook inside bus_event: a read's event is already stored, a
 * write's comes after its hooks and is left out. */
BUS_HOT void bus_trace_freeze_hot(void) {
    if (!trace_frozen) { trace_end = trace_seq; trace_frozen = true; }
}

size_t bus_trace_copy(bus_trace_entry *out, size_t max) {
    uint32_t end = trace_frozen ? trace_end : trace_seq;
    uint32_t avail = end - trace_start;
    if (avail > BUS_TRACE_SIZE) avail = BUS_TRACE_SIZE;
    uint32_t n = max < avail ? (uint32_t)max : avail;
    size_t got = 0;
    for (uint32_t s = end - n; s != end; s++) {
        uint32_t w = trace_ring[s & (BUS_TRACE_SIZE - 1)];
        if ((w & 0xFF800000u) != TRACE_TAG(s)) continue;   /* overwritten while running, or never stored: skip */
        out[got++] = (bus_trace_entry){ .seq = s, .idx = BUS_EV_IDX(w), .rw = (uint8_t)BUS_EV_RD(w), .data = BUS_EV_DATA(w) };
    }
    return got;
}

/* hook_lo keeps ROM fetches out of the hook scan: every hook so far sits at
 * $FF40 and up. */
static inline __attribute__((always_inline)) void run_read_hooks(uint16_t idx) {
    if (idx < hook_lo) return;
    for (int i = 0; i < hook_count; i++) {
        if (hooks[i].idx == idx) { hooks[i].fn(); return; }   /* one hook per address */
    }
}

static inline __attribute__((always_inline)) void run_write_hooks(uint16_t idx, uint8_t data) {
    for (int i = 0; i < whook_count; i++) {
        if (whooks[i].idx == idx) { whooks[i].fn(data); bus_stats.whooks_run++; }
    }
}

static inline __attribute__((always_inline)) void queue_write(uint16_t idx, uint8_t data) {
    uint32_t h = wev_head, n = (h + 1) & WEV_MASK;
    if (n == wev_tail) {
        bus_stats.write_overrun++;
    } else {
        wev[h].idx = idx;
        wev[h].data = data;
        __atomic_store_n(&wev_head, n, __ATOMIC_RELEASE);
    }
}

static inline __attribute__((always_inline)) void trace_store(uint32_t w) {
    uint32_t n = trace_seq;
    trace_seq = n + 1;
    if (!trace_frozen) trace_ring[n & (BUS_TRACE_SIZE - 1)] = w | TRACE_TAG(n);   /* w's bits 23-31 are 0; if not, the copy's tag check drops it */
}

/* One selected cycle (core1's event loop; the host simulator). A write runs
 * its hooks first: a $FF40 bank switch races the next cycle's `in x, 3`, so
 * nothing goes in front of it; they also run before the write is queued, so
 * core0 sees the effect of a hook no later than the write itself. A read
 * keeps its order (the trace store, then the hooks, so a hook that freezes
 * the trace keeps its own event) and its own path: with the Becker hooks at
 * 1.79 MHz core1 has only a few clk per event to spare, and a shared path
 * cost enough to lap the ring. */
BUS_HOT void bus_event(uint32_t w) {
    uint16_t idx = BUS_EV_IDX(w);
    if (!BUS_EV_RD(w)) {
        run_write_hooks(idx, BUS_EV_DATA(w));
        trace_store(w);
        bus_stats.cycles++;
        bus_stats.writes++;
        queue_write(idx, BUS_EV_DATA(w));
        return;
    }
    trace_store(w);
    bus_stats.cycles++;
    bus_stats.reads++;
    run_read_hooks(idx);
}
