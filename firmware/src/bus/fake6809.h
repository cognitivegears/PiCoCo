#pragma once
/* On-chip fake 6809: PIO1 drives A0-A13, R/W and OE_BUS (Pico 2) or E and
 * the selects (Plus-W) with 6809 timing so the engine can be tested with no
 * CoCo attached. */

/* `bus selftest` - DMA-fed back-to-back cycles at 1.79 and 0.89 MHz timing
 * (fake6809_fast in fake6809.pio). 0 pass, -1 fail, -2 refused: bus live. */
#define FAST_OPT_STRESS 1   /* core0 memcpy loop during every burst */
#define FAST_OPT_RADIO  2   /* Plus-W: WiFi scans + cyw43 polling during every burst */
#define FAST_OPT_RESTARTS 4 /* stop after 40 more engine-restart bursts */
#define FAST_OPT_SWITCHES 8 /* stop after 40 more bank-switch bursts */
int fake6809_fast(int opts, void (*line)(const char *s));
