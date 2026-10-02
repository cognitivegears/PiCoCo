#include "becker.h"
#include "device.h"
#include "bus.h"
#include "ring.h"

#define TO_COCO_SIZE   256   /* 255 usable */
#define FROM_COCO_SIZE 1024  /* 1023 usable */
#define BECKER_LO 0x3F40
#define BECKER_HI 0x3F5F

becker_stats_t becker_stats;

static uint8_t to_coco_buf[TO_COCO_SIZE];
static uint8_t from_coco_buf[FROM_COCO_SIZE];
static ring_t to_coco, from_coco;

/* ponytail: single writer — only core1 (the two read hooks below) writes
 * the 0x3F41/0x3F42 entries (through bus_io_set, every bank); core0 (becker_write/becker_loopback_pump) only
 * pushes into the ring. Two writers racing on those table entries could
 * publish a stale data byte alongside a fresh "ready" status and drop a
 * byte. Ceiling: a freshly pushed byte becomes visible on the poll after
 * the push — one extra $FF41 poll of latency, no lost bytes. */
/* The engine serves a read between any two of these stores, so the order is
 * the protocol: publishing a byte, data then status (a poll in between sees
 * "not ready"); emptying, status then data (never "ready" over a stale byte). */
BUS_HOT void becker_refresh(void) {
    uint8_t b;
    /* bus_io_set: the entries are mirrored in every bank (bus.h). */
    if (ring_peek(&to_coco, &b)) {
        bus_io_set(BUS_IDX_BECKER_DATA, b);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* the order above: kept here, not by bus_io_set's being in another file */
        bus_io_set(BUS_IDX_BECKER_STATUS, 0x02);
    } else {
        bus_io_set(BUS_IDX_BECKER_STATUS, 0x00);
        __atomic_signal_fence(__ATOMIC_SEQ_CST);
        bus_io_set(BUS_IDX_BECKER_DATA, 0xFF);
    }
}

static BUS_HOT void becker_status_hook(void) {
    becker_refresh();
}

/* Runs inside the bus cycle on core1. A CoCo 1/2 fetches its next opcode from
 * the cart 1.12 us after this read, so the pop and the refresh are one pass
 * over the ring here (bench 2026-10-01: ring_pop() then becker_refresh() was
 * ~120 clk_sys cycles and the loop missed that fetch). A byte core0 pushes
 * after the head was read shows up on the next $FF41 poll, as before. */
static BUS_HOT void becker_data_hook(void) {
    uint32_t t = to_coco.tail;
    uint32_t h = __atomic_load_n(&to_coco.head, __ATOMIC_ACQUIRE);
    if (bus_table[BUS_IDX_BECKER_STATUS] == 0x02 && h != t) {
        t = (t + 1) & to_coco.mask;
        __atomic_store_n(&to_coco.tail, t, __ATOMIC_RELEASE);
        becker_stats.reads++;
        if (h != t) {                        /* status stays 0x02 */
            bus_io_set(BUS_IDX_BECKER_DATA, to_coco.buf[t]);
        } else {
            bus_io_set(BUS_IDX_BECKER_STATUS, 0x00);
            __atomic_signal_fence(__ATOMIC_SEQ_CST);   /* status then data, as in becker_refresh */
            bus_io_set(BUS_IDX_BECKER_DATA, 0xFF);
        }
        return;
    }
    becker_stats.underrun++;
    bus_trace_freeze_hot();                  /* keep the cycles that led here; `trace run` thaws */
    becker_refresh();
}

static void (*ctl_fn)(uint8_t code);

void becker_set_ctl(void (*fn)(uint8_t code)) { ctl_fn = fn; }

static void becker_on_write(uint16_t idx, uint8_t data) {
    if (idx == BUS_IDX_BECKER_CTL) {
        uint8_t b;
        if (data == 0xA5) while (ring_pop(&from_coco, &b)) { }   /* core0 is this ring's only consumer */
        if (ctl_fn) ctl_fn(data);
        return;
    }
    if (idx != BUS_IDX_BECKER_DATA) return;
    if (ring_push(&from_coco, data)) {
        becker_stats.writes++;
    } else {
        becker_stats.overrun++;
    }
}

static const device_t becker_device = {
    "becker", BECKER_LO, BECKER_HI, NULL, becker_on_write
};

void becker_init(void) {
    ring_init(&to_coco, to_coco_buf, TO_COCO_SIZE);
    ring_init(&from_coco, from_coco_buf, FROM_COCO_SIZE);
    becker_stats = (becker_stats_t){ 0 };
    device_register(&becker_device);
    bus_add_read_hook(BUS_IDX_BECKER_STATUS, becker_status_hook);
    bus_add_read_hook(BUS_IDX_BECKER_DATA, becker_data_hook);
    becker_refresh();
}

size_t becker_read(uint8_t *buf, size_t n) {
    size_t i = 0;
    for (; i < n; i++) {
        if (!ring_pop(&from_coco, &buf[i])) break;
    }
    return i;
}

size_t becker_write(const uint8_t *buf, size_t n) {
    size_t i = 0;
    for (; i < n; i++) {
        if (!ring_push(&to_coco, buf[i])) break;
    }
    return i;
}

size_t becker_rx_avail(void) { return ring_count(&from_coco); }
size_t becker_tx_free(void) { return ring_free(&to_coco); }

void becker_loopback_pump(void) {
    uint8_t b;
    while (ring_free(&to_coco) > 0 && ring_pop(&from_coco, &b)) {
        ring_push(&to_coco, b);
    }
}
