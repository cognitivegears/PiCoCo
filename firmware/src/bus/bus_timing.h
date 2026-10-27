#pragma once
/* The bus engine's timing, all derived from one clock constant so a clock
 * change recomputes every count (tests/test_timing.c checks the budget
 * before one). Counts are PIO clk; the engine's SMs run at clk_sys
 * (bus_engine.c). Census: docs/firmware-architecture.md §8. */
#ifndef BUS_SYS_CLK_HZ
#define BUS_SYS_CLK_HZ 150000000u        /* the SDK default; the RP2350 is rated for 150 */
#endif
#define BUS_PIO_CLKDIV 1                 /* integer: a divider >1 or fractional stretches the 22-clk count */
#define BUS_PIO_HZ (BUS_SYS_CLK_HZ / BUS_PIO_CLKDIV)
#define BUS_NS_TO_CLK(ns) ((int)((unsigned long long)(ns) * BUS_PIO_HZ / 1000000000u))
#define BUS_CLK_TO_NS(clk) ((int)((unsigned long long)(clk) * 1000000000u / BUS_PIO_HZ))

#define BUS_SERVE_CLK 22                 /* OE_BUS falls -> byte on D0-D7, Pico 2, measured 2026-10-02 */
#define BUS_SERVE_NS BUS_CLK_TO_NS(BUS_SERVE_CLK)

/* The realistic sample point (§8): 240 ns after OE_BUS falls at 1.79 MHz
 * (560 ns cycle); everything doubles at 0.89 MHz. */
#define BUS_CYCLE_NS_FAST 560
#define BUS_LATCH_NS_FAST 240
#define BUS_LATCH_NS_SLOW (2 * BUS_LATCH_NS_FAST)
#define BUS_CYCLE_CLK_FAST BUS_NS_TO_CLK(BUS_CYCLE_NS_FAST)
#define BUS_LATCH_CLK_FAST BUS_NS_TO_CLK(BUS_LATCH_NS_FAST)
#define BUS_LATCH_CLK_SLOW BUS_NS_TO_CLK(BUS_LATCH_NS_SLOW)
#define BUS_MARGIN_CLK_FAST (BUS_LATCH_CLK_FAST - BUS_SERVE_CLK)
#define BUS_MARGIN_CLK_SLOW (BUS_LATCH_CLK_SLOW - BUS_SERVE_CLK)
