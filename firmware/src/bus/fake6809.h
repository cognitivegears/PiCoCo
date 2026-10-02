#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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

/* Building blocks for other on-device self-tests (net_selftest.c): the same
 * guards and pin takeover fake6809_selftest uses, one selected cycle at a
 * time. Never with a CoCo attached. */
int     fake6809_begin(void);                                   /* 0 ok, -1 no PIO SM, -2 refused: bus live */
uint8_t fake6809_cycle(uint16_t addr, bool rd, uint8_t data);   /* rd: byte core1 drove; write: data goes to core1 */
void    fake6809_end(void);

/* Pico 2: `bus selftest fast` - DMA-fed back-to-back reads at 1.79 and 0.89 MHz
 * timing (fake6809_fast in fake6809.pio). 0 pass, -1 fail, -2 refused: bus live. */
#define FAST_OPT_STRESS 1   /* core0 memcpy loop during every burst */
#define FAST_OPT_RADIO  2   /* Plus-W: WiFi scans + cyw43 polling during every burst */
#define FAST_OPT_RESTARTS 4 /* stop after 40 more engine-restart bursts */
#define FAST_OPT_SWITCHES 8 /* stop after 40 more bank-switch bursts */
int fake6809_fast(int opts, void (*line)(const char *s));
