#pragma once
#include <stdint.h>
#include <stddef.h>
/* On-chip fake 6809: PIO1 drives A0-A13, R/W and OE_BUS (Pico 2) with 6809
 * timing so core1's real loop can be tested with no CoCo attached. */
typedef struct {
    uint32_t cycles, mismatches, ring_overrun;
    int first_ok_delay;          /* smallest sample delay (PIO cycles after OE fell) with correct read data, -1 none */
    uint32_t delay_ns;           /* first_ok_delay in ns at the PIO clock */
    int burst_first_ok_delay;    /* Plus-W: smallest sample delay with a mismatch-free back-to-back burst, -1 none */
    uint32_t burst_delay_ns;     /* burst_first_ok_delay in ns at the PIO clock */
} fake_result_t;
int  fake6809_selftest(fake_result_t *r, void (*line)(const char *s));   /* 0 pass, -1 fail, -2 refused: bus live */
