#include "device.h"
#include "bus.h"

static device_t devices[DEVICE_MAX];
static int device_count;

int device_register(const device_t *d) {
    if (device_count >= DEVICE_MAX) return -1;
    devices[device_count++] = *d;
    return 0;
}

void device_init_all(void) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].init) devices[i].init();
    }
}

void device_reset(void) {
    device_count = 0;
}

size_t device_dispatch_writes(void) {
    size_t n = 0;
    uint16_t idx;
    uint8_t data;
    while (bus_pop_write(&idx, &data)) {
        n++;
        for (int i = 0; i < device_count; i++) {
            if (idx >= devices[i].idx_lo && idx <= devices[i].idx_hi) {
                if (devices[i].on_write) devices[i].on_write(idx, data);
                break;
            }
        }
    }
    return n;
}
