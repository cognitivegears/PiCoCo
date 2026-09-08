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

BUS_HOT void becker_refresh(void) {
    uint8_t b;
    if (ring_peek(&to_coco, &b)) {
        bus_set_read(BUS_IDX_BECKER_STATUS, 0x02);
        bus_set_read(BUS_IDX_BECKER_DATA, b);
    } else {
        bus_set_read(BUS_IDX_BECKER_STATUS, 0x00);
        bus_set_read(BUS_IDX_BECKER_DATA, 0xFF);
    }
}

static BUS_HOT void becker_read_hook(void) {
    uint8_t b;
    if (ring_pop(&to_coco, &b)) {
        becker_stats.reads++;
    } else {
        becker_stats.underrun++;
    }
    becker_refresh();
}

static void becker_on_write(uint16_t idx, uint8_t data) {
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
    bus_add_read_hook(BUS_IDX_BECKER_DATA, becker_read_hook);
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
    becker_refresh();
    return i;
}

size_t becker_rx_avail(void) { return ring_count(&from_coco); }
size_t becker_tx_free(void) { return ring_free(&to_coco); }

void becker_loopback_pump(void) {
    uint8_t b;
    while (ring_free(&to_coco) > 0 && ring_pop(&from_coco, &b)) {
        ring_push(&to_coco, b);
    }
    becker_refresh();
}
