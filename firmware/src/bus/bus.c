#include "bus.h"
#include <string.h>

uint8_t bus_table[BUS_TABLE_SIZE];
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

typedef struct { uint16_t idx; void (*fn)(uint8_t); } bus_whook_t;
static bus_whook_t whooks[BUS_MAX_HOOKS];
static int whook_count;
static bool in_whook;   /* ponytail: true while a write hook is executing; makes bus_pop_write non-destructive */

static bus_trace_entry trace[BUS_TRACE_SIZE];
static uint32_t trace_pos;
static bool trace_frozen;

void bus_init(void) {
    memset(bus_table, 0xFF, sizeof(bus_table));
    wev_head = 0;
    wev_tail = 0;
    hook_count = 0;
    whook_count = 0;
    in_whook = false;
    trace_pos = 0;
    trace_frozen = false;
    bus_stats.cycles = 0;
    bus_stats.reads = 0;
    bus_stats.writes = 0;
    bus_stats.write_overrun = 0;
    bus_stats.whooks_run = 0;
    bus_stats.hw_selected = 0;
    bus_stats.fw_selected = 0;
    bus_drive = false;
}

void bus_drive_set(bool on) { bus_drive = on; }
bool bus_drive_get(void) { return bus_drive; }

BUS_HOT void bus_set_read(uint16_t idx, uint8_t v) {
    if (idx >= BUS_TABLE_SIZE) return;
    bus_table[idx] = v;
}

void bus_set_read_range(uint16_t idx, const uint8_t *p, size_t n) {
    if (idx >= BUS_TABLE_SIZE) return;
    size_t max = BUS_TABLE_SIZE - idx;
    size_t cnt = n < max ? n : max;
    memcpy(&bus_table[idx], p, cnt);
}

int bus_add_read_hook(uint16_t idx, void (*fn)(void)) {
    if (hook_count >= BUS_MAX_HOOKS) return -1;
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
    if (!in_whook) __atomic_store_n(&wev_tail, (t + 1) & WEV_MASK, __ATOMIC_RELEASE);
    return true;
}

void bus_trace_freeze(bool freeze) {
    trace_frozen = freeze;
}

size_t bus_trace_copy(bus_trace_entry *out, size_t max) {
    size_t avail = trace_pos < BUS_TRACE_SIZE ? trace_pos : BUS_TRACE_SIZE;
    size_t n = max < avail ? max : avail;
    uint32_t start = trace_pos - (uint32_t)n;
    for (size_t i = 0; i < n; i++) out[i] = trace[(start + i) & (BUS_TRACE_SIZE - 1)];
    return n;
}

static BUS_HOT void trace_record(uint16_t idx, uint8_t rw, uint8_t data, uint32_t t_us) {
    if (trace_frozen) return;
    trace[trace_pos & (BUS_TRACE_SIZE - 1)] = (bus_trace_entry){ .t_us = t_us, .idx = idx, .rw = rw, .data = data };
    trace_pos++;
}

BUS_HOT void bus_on_read_done(uint16_t idx, uint32_t t_us) {
    uint8_t data = bus_table[idx];   /* what the table holds, i.e. what would have been driven, even with bus drive off */
    for (int i = 0; i < hook_count; i++) {
        if (hooks[i].idx == idx) hooks[i].fn();
    }
    trace_record(idx, 1, data, t_us);
    bus_stats.cycles++;
    bus_stats.reads++;
}

BUS_HOT void bus_on_write(uint16_t idx, uint8_t data, uint32_t t_us) {
    for (int i = 0; i < whook_count; i++) {
        if (whooks[i].idx == idx) {
            in_whook = true;
            whooks[i].fn(data);
            in_whook = false;
            bus_stats.whooks_run++;
        }
    }
    uint32_t h = wev_head, n = (h + 1) & WEV_MASK;
    if (n == wev_tail) {
        bus_stats.write_overrun++;
    } else {
        wev[h].idx = idx;
        wev[h].data = data;
        __atomic_store_n(&wev_head, n, __ATOMIC_RELEASE);
    }
    trace_record(idx, 0, data, t_us);
    bus_stats.cycles++;
    bus_stats.writes++;
}
