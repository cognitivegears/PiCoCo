#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *name;
    uint16_t idx_lo, idx_hi;
    void (*init)(void);
    void (*on_write)(uint16_t idx, uint8_t data);
} device_t;

#define DEVICE_MAX 8

int    device_register(const device_t *d);  /* 0 ok, -1 full */
void   device_init_all(void);
void   device_reset(void);                   /* clears registry (tests) */
size_t device_dispatch_writes(void);         /* drains bus write ring to on_write by range; returns events handled */
